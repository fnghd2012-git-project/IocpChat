#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include "../Common.h"

#pragma comment(lib, "Ws2_32.lib")

// 입력은 메인에서 받고, 다른 사람이 보낸 채팅은 수신 쓰레드가 바로 출력한다.
void PrintLine(const string& line)
{
    static mutex outputMutex;
    lock_guard<mutex> lock(outputMutex);
    cout << line << endl;
}

struct stClientResources
{
    bool winsockStarted = false;
    SOCKET socket = INVALID_SOCKET;
    atomic_bool connected{ false };
    thread receiver;

    // 수신 쓰레드가 소켓을 놓은 뒤에 닫는다. 재접속도 같은 순서로 정리한다.
    void CloseConnection()
    {
        connected = false;
        if (socket != INVALID_SOCKET)
            shutdown(socket, SD_BOTH);
        if (receiver.joinable())
            receiver.join();
        if (socket != INVALID_SOCKET)
        {
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
    }

    ~stClientResources()
    {
        CloseConnection();
        if (winsockStarted)
            WSACleanup();
    }
};

void RunChatReceiver(SOCKET socket, atomic_bool& connected)
{
    string pending;
    char buffer[MaxChatBytes];
    while (connected)
    {
        // B가 아무것도 입력하지 않아도 이 쓰레드는 서버가 B에게 보낸 채팅을 기다린다.
        const int bytes = recv(socket, buffer, sizeof(buffer), 0);
        if (bytes <= 0)
            break;

        pending.append(buffer, bytes);
        string line;
        LineResult result;
        // 줄바꿈까지 받은 채팅만 화면에 찍고, 덜 온 다음 줄은 pending에 남겨 둔다.
        while ((result = TakeChatLine(pending, MaxServerLineBytes, line)) == LineResult::Ready)
            PrintLine(line);
        if (result == LineResult::TooLong)
        {
            PrintLine("서버에서 받은 채팅 한 줄이 너무 깁니다.");
            break;
        }
    }
    if (connected.exchange(false))
        PrintLine("서버 연결이 끊어졌습니다. /reconnect로 다시 접속할 수 있습니다.");
}

bool ConnectToServer(stClientResources& client)
{
    client.CloseConnection();

    client.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client.socket == INVALID_SOCKET)
    {
        cerr << "클라이언트 소켓 생성 실패. 오류 코드: " << WSAGetLastError() << '\n';
        return false;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(9000);
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(client.socket, reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        client.CloseConnection();
        cerr << "서버 접속 실패. 오류 코드: " << error
            << "\n서버 실행 후 /reconnect로 다시 접속할 수 있습니다.\n";
        return false;
    }

    // 창마다 다른 로컬 포트를 표시해서 연결을 구분한다.
    sockaddr_in localAddress{};
    int addressBytes = sizeof(localAddress);
    if (getsockname(client.socket, reinterpret_cast<sockaddr*>(&localAddress),
        &addressBytes) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        client.CloseConnection();
        cerr << "클라이언트 주소 확인 실패. 오류 코드: " << error << '\n';
        return false;
    }

    cout << "서버 접속 완료: 127.0.0.1:9000 / 내 포트: "
        << ntohs(localAddress.sin_port) << endl;
    client.connected = true;
    try
    {
        client.receiver = thread(RunChatReceiver, client.socket, ref(client.connected));
    }
    catch (const system_error&)
    {
        client.CloseConnection();
        cerr << "채팅 수신 쓰레드를 시작하지 못했습니다.\n";
        return false;
    }
    return true;
}

// 입력은 최대 4096바이트라 int로 크기를 계산한다.
// 일부만 보내졌으면 남은 부분을 이어서 보낸다.
bool SendAll(SOCKET socket, const string& message)
{
    const int messageBytes = static_cast<int>(message.size());
    int sentBytes = 0;

    while (sentBytes < messageBytes)
    {
        const int bytes = send(socket, message.data() + sentBytes,
            messageBytes - sentBytes, 0);

        if (bytes == SOCKET_ERROR)
        {
            cerr << "서버로 데이터 전송 실패. 오류 코드: " << WSAGetLastError() << '\n';
            return false;
        }
        if (bytes == 0)
        {
            cerr << "전송한 데이터가 0바이트입니다. 연결을 종료합니다.\n";
            return false;
        }

        sentBytes += bytes;
    }

    return true;
}

int main()
{
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    const DWORD processId = GetCurrentProcessId();
    const wstring title = L"IOCP 채팅 - " + to_wstring(processId);
    SetConsoleTitleW(title.c_str());

    stClientResources client;
    WSADATA wsaData{};
    // 소켓 사용 전 Winsock 초기화.
    const int startupError = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupError != 0)
    {
        cerr << "네트워크 초기화 실패. 오류 코드: " << startupError << '\n';
        return 1;
    }
    client.winsockStarted = true;

    cout << "클라이언트 프로세스: " << processId << '\n';
    ConnectToServer(client);
    PrintLine("한 줄 입력하면 다른 사람들에게 전송됩니다. 최대 " + to_string(MaxChatBytes) + "바이트입니다.");
    PrintLine("/repeat 횟수 메시지 (1~100회), /reconnect 재접속, /quit 종료");

    string message;
    while (true)
    {
        if (!getline(cin, message) || message == "/quit")
            break;

        if (message == "/reconnect")
        {
            ConnectToServer(client);
            continue;
        }

        // /repeat는 이 창에서 처리한다. 서버에는 사용자가 입력한 본문만 보낸다.
        int repeatCount = 1;
        if (message == "/repeat" || message.compare(0, 8, "/repeat ") == 0)
        {
            istringstream arguments(message.substr(7));
            string countText;
            arguments >> countText;
            istringstream countValue(countText);
            if (!(countValue >> repeatCount) ||
                countValue.peek() != char_traits<char>::eof() ||
                repeatCount < 1 || repeatCount > 100)
            {
                PrintLine("사용법: /repeat 10 안녕하세요 (횟수: 1~100)");
                continue;
            }
            string repeatedMessage;
            getline(arguments >> ws, repeatedMessage);
            if (repeatedMessage.empty())
            {
                PrintLine("반복 전송할 메시지를 입력하세요. 예: /repeat 10 안녕하세요");
                continue;
            }
            message = repeatedMessage;
        }
        if (message.empty())
            continue;
        if (!client.connected)
        {
            PrintLine("연결된 서버가 없습니다. /reconnect로 접속하세요.");
            continue;
        }
        if (message.size() > MaxChatBytes)
        {
            PrintLine("입력한 메시지가 너무 깁니다. 최대 " + to_string(MaxChatBytes) + "바이트까지 보낼 수 있습니다.");
            continue;
        }

        // 엔터 하나가 채팅 한 줄이다. 서버가 다음 줄과 구분할 수 있게 줄바꿈도 함께 보낸다.
        const string line = message + "\n";
        int sentCount = 0;
        while (sentCount < repeatCount && client.connected)
        {
            if (!SendAll(client.socket, line))
            {
                client.CloseConnection();
                PrintLine("전송을 중단했습니다. /reconnect로 다시 접속할 수 있습니다.");
                break;
            }
            ++sentCount;
        }
        if (repeatCount > 1)
            PrintLine("반복 전송: " + to_string(sentCount) + "/" + to_string(repeatCount));
    }

    client.CloseConnection();
    PrintLine("채팅을 종료했습니다.");
    return 0;
}
