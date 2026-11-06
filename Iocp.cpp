#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "Common.h"
#include "JobQueue.h"

#pragma comment(lib, "Ws2_32.lib")

// 메인은 접속을 받고, IOCP 쓰레드는 송수신을 맡고, 로직 쓰레드는 채팅을 다른 접속에 배분한다.
constexpr int IocpWorkerCount = 2;
constexpr int LogicWorkerCount = 2;
constexpr size_t MaxQueuedSendBytes = 256 * 1024;

struct stSession
{
    unsigned int id = 0;
    SOCKET socket = INVALID_SOCKET;
    mutex sessionMutex;
    bool inRoom = false; // 첫 수신을 준비하고 내 번호를 송신 큐에 넣은 뒤부터 채팅을 받는다.
    bool closing = false;

    OVERLAPPED recvOverlapped{};
    bool recvPending = false;
    char recvBuffer[MaxChatBytes]{};
    string recvText; // 한 번 수신한 뒤에도 줄바꿈이 올 때까지 남겨 둔다.

    OVERLAPPED sendOverlapped{};
    bool sendPending = false;
    queue<string> sendQueue; // 맨 앞 문자열은 WSASend가 끝날 때까지 그대로 둔다.
    size_t sendOffset = 0;
    size_t queuedSendBytes = 0; // 부분 송신 중인 줄을 포함해서 큐가 보관 중인 바이트.

    ~stSession()
    {
        if (socket != INVALID_SOCKET)
            closesocket(socket);
    }
};

// 워커 함수 안에 따로 만들지 않고 모든 워커에 같은 객체를 전달한다.
struct stWorkerContext
{
    HANDLE completionPort = nullptr;                           // 모든 IOCP 워커가 사용하는 IOCP
    CJobQueue jobQueue;                                        // 직접 구현한 공용 작업 큐
    mutex sessionsMutex;                                       // 세션 목록만 잠깐 보호
    mutex broadcastMutex;                                         // 방 안의 접속들이 같은 순서로 채팅을 받게 한다.
    condition_variable sessionsEmpty;                          // 남은 I/O 정리가 끝났는지 대기
    unordered_map<stSession*, shared_ptr<stSession>> sessions; // 접속 목록
};

// 서버가 직접 넣은 알림은 소켓의 완료 통지와 달리 OVERLAPPED가 없다.
enum class Notice
{
    NewSession,
    ReceiveNext,
    StartSend
};

struct stNotice
{
    Notice type;
    shared_ptr<stSession> sp_session;
};

// 소켓과 IOCP 핸들을 정리한다. 초기화 중간에 실패해도 생성된 것만 정리한다.
// listener는 접속 대기용이고, 클라이언트 소켓은 각 세션이 관리한다.
// 진행 중인 I/O가 있으면 완료 통지까지 받은 뒤에 버퍼와 세션을 정리해야 한다.
struct stServerResources
{
    bool winsockStarted = false;      // WSAStartup에 성공했을 때만 WSACleanup을 호출한다.
    SOCKET listener = INVALID_SOCKET; // 메인 쓰레드가 accept에서 기다리는 소켓
    HANDLE completionPort = nullptr;  // 모든 워커가 끝난 뒤 닫을 IOCP 핸들

    ~stServerResources()
    {
        if (listener != INVALID_SOCKET)
            closesocket(listener);

        if (completionPort != nullptr)
            CloseHandle(completionPort);

        if (winsockStarted)
            WSACleanup();
    }
};

// 요청이 정상 등록됐는지만 확인한다. 실제 완료 처리는 RunIocpWorker에서 한다.
bool CheckIoResult(int result)
{
    // 바로 완료됐어도 IOCP 통지가 들어온다. 여기서 처리하면 같은 작업을 두 번 처리하게 된다.
    if (result == 0)
        return true;

    const int error = WSAGetLastError();
    // 요청은 등록됐고 완료를 기다리는 상태. 실패로 처리하면 안 된다.
    if (error == WSA_IO_PENDING)
        return true;

    // 나머지는 등록 실패. 완료 통지가 오지 않으므로 기다리지 않는다.
    cerr << "비동기 입출력 요청 등록 실패. 오류 코드: " << error << '\n';
    return false;
}

// sessionMutex를 잡고 등록한다. 즉시 완료돼도 다른 워커가 이 상태를 먼저 바꾸지 못한다.
bool PostRecv(stSession& session)
{
    session.recvOverlapped = {};
    WSABUF buffer{ static_cast<ULONG>(sizeof(session.recvBuffer)), session.recvBuffer };
    DWORD flags = 0;
    session.recvPending = CheckIoResult(WSARecv(session.socket, &buffer, 1,
        nullptr, &flags, &session.recvOverlapped, nullptr));
    return session.recvPending;
}

// 한 접속에는 송신 하나만 등록한다. 일부만 보냈으면 같은 줄의 남은 부분부터 보낸다.
bool PostSend(stSession& session)
{
    // B에게 보낼 차례면 session은 B다. 맨 앞줄을 WSASend에 넘기고, 송신 완료가 올 때까지 큐에 둔다.
    string& front = session.sendQueue.front();
    session.sendOverlapped = {};
    WSABUF buffer{ static_cast<ULONG>(front.size() - session.sendOffset), &front[session.sendOffset] };
    session.sendPending = CheckIoResult(WSASend(session.socket, &buffer, 1,
        nullptr, 0, &session.sendOverlapped, nullptr));
    return session.sendPending;
}

// 여러 스레드의 로그가 한 줄 안에서 섞이지 않게 한다.
void Log(const string& message)
{
    static mutex logMutex;
    lock_guard<mutex> lock(logMutex);
    cout << message << endl;
}

shared_ptr<stSession> FindSession(stWorkerContext& context, stSession* p_sessionKey)
{
    lock_guard<mutex> lock(context.sessionsMutex);
    const auto found = context.sessions.find(p_sessionKey);
    return found == context.sessions.end() ? nullptr : found->second;
}

// 호출자가 sessionMutex를 잡고 들어온다. 소켓을 닫은 뒤에도 마지막 I/O 완료가 올 수 있다.
void CloseSessionLocked(stWorkerContext& context, stSession& session)
{
    if (!session.closing)
    {
        session.closing = true;
        closesocket(session.socket);
        session.socket = INVALID_SOCKET;
        Log("[접속 " + to_string(session.id) + "] 연결 종료");
    }

    // 소켓을 닫아도 아직 완료되지 않은 I/O의 버퍼는 유지한다.
    if (!session.recvPending && !session.sendPending)
    {
        lock_guard<mutex> lock(context.sessionsMutex);
        context.sessions.erase(&session);
        context.sessionsEmpty.notify_all();
    }
}

bool PostNotice(stWorkerContext& context, Notice type, const shared_ptr<stSession>& sp_session)
{
    // QueueSendLocked에서 PostNotice(context, Notice::StartSend, B)를 호출하면 type은 StartSend, sp_session은 B다.
    auto* p_notice = new stNotice{ type, sp_session };
    // 알림 주소를 완료 키로 넣는다. 성공하면 RunIocpWorker가 같은 주소를 받아 HandleNotice에 넘긴다.
    if (PostQueuedCompletionStatus(context.completionPort, 0, reinterpret_cast<ULONG_PTR>(p_notice), nullptr))
        return true;

    const DWORD error = GetLastError();
    delete p_notice;
    Log("IOCP 알림 등록 실패. 오류 코드: " + to_string(error));
    return false;
}

// 호출자가 받는 사람의 sessionMutex를 잡고 있다. 빈 큐에 첫 줄이 들어올 때만 IOCP 워커를 깨운다.
void QueueSendLocked(stWorkerContext& context, const shared_ptr<stSession>& sp_session, const string& line)
{
    if (sp_session->queuedSendBytes + line.size() > MaxQueuedSendBytes)
    {
        Log("접속 " + to_string(sp_session->id) + " 송신 대기량 초과");
        CloseSessionLocked(context, *sp_session);
        return;
    }

    const bool startSend = sp_session->sendQueue.empty();
    // 예: A가 쓴 "[사용자 1] 안녕"을 B에게 보낼 차례라면, 여기는 B의 송신 큐다. line 한 줄을 복사해 둔다.
    sp_session->sendQueue.push(line);
    sp_session->queuedSendBytes += line.size();
    // 방금 넣은 줄이 첫 줄이면 StartSend를 올린다. 앞에 줄이 있으면 그 줄의 송신 완료 처리에서 이어서 보낸다.
    if (startSend && !PostNotice(context, Notice::StartSend, sp_session))
        CloseSessionLocked(context, *sp_session);
}

// 같은 사람이 보낸 두 줄을 로직 워커 두 개가 거꾸로 처리하지 않게 한 줄씩 넘긴다.
void ContinueReceiveLocked(stWorkerContext& context, const shared_ptr<stSession>& sp_session)
{
    string line;
    while (true)
    {
        const LineResult result = TakeChatLine(sp_session->recvText, MaxChatBytes, line);
        if (result == LineResult::Incomplete)
        {
            if (!PostRecv(*sp_session))
                CloseSessionLocked(context, *sp_session);
            return;
        }
        if (result == LineResult::TooLong)
        {
            Log("접속 " + to_string(sp_session->id) + " 채팅 한 줄의 길이 초과");
            CloseSessionLocked(context, *sp_session);
            return;
        }
        if (line.empty())
            continue;

        // 여기서 sp_session은 보낸 사람 A다. A와 완성된 한 줄을 공용 큐에 넣으면 RunLogicWorker가 꺼낸다.
        if (!context.jobQueue.Push({ sp_session, move(line) }))
            CloseSessionLocked(context, *sp_session);
        return; // 로직 워커가 ReceiveNext를 보내면 남아 있는 다음 줄부터 이어서 본다.
    }
}

void HandleNotice(stWorkerContext& context, const stNotice& notice)
{
    const auto& sp_session = notice.sp_session;
    lock_guard<mutex> lock(sp_session->sessionMutex);
    if (sp_session->closing)
        return;

    // PostNotice에 두 번째 인자로 넘긴 값이 notice.type이다. 그 값에 따라 이 세션에서 할 일이 정해진다.
    switch (notice.type)
    {
    case Notice::NewSession:
        QueueSendLocked(context, sp_session, "채팅방에 입장했습니다. 내 번호: " + to_string(sp_session->id) + "\n");
        if (sp_session->closing)
            return;
        sp_session->inRoom = true;
        ContinueReceiveLocked(context, sp_session);
        break;
    case Notice::ReceiveNext:
        ContinueReceiveLocked(context, sp_session);
        break;
    case Notice::StartSend:
        // StartSend에 담긴 세션이 B라면 PostSend가 B의 소켓으로 WSASend를 등록한다.
        if (!PostSend(*sp_session))
            CloseSessionLocked(context, *sp_session);
        break;
    }
}

// 완료에 돌아온 OVERLAPPED 주소를 보면 같은 접속에서도 수신과 송신을 구분할 수 있다.
void HandleSessionCompletion(stWorkerContext& context, stSession* p_sessionKey, OVERLAPPED* p_overlapped,
    DWORD bytes, BOOL success, DWORD error, const string& worker)
{
    auto sp_session = FindSession(context, p_sessionKey);
    if (!sp_session)
        terminate();

    lock_guard<mutex> lock(sp_session->sessionMutex);
    const bool received = p_overlapped == &sp_session->recvOverlapped;
    if (received)
        sp_session->recvPending = false;
    else if (p_overlapped == &sp_session->sendOverlapped)
        sp_session->sendPending = false;
    else
        terminate();

    if (!success || bytes == 0 || sp_session->closing)
    {
        if (!success && !sp_session->closing)
            Log(worker + "입출력 실패. 오류 코드: " + to_string(error));
        CloseSessionLocked(context, *sp_session);
        return;
    }

    if (received)
    {
        Log(worker + "접속 " + to_string(sp_session->id) + " 수신 완료 " + to_string(bytes) + "바이트");
        // 한 번에 받은 바이트를 이어 붙인다. 줄바꿈이 올 때까지는 같은 사람의 recvText에 남겨 둔다.
        sp_session->recvText.append(sp_session->recvBuffer, bytes);
        ContinueReceiveLocked(context, sp_session);
        return;
    }

    sp_session->sendOffset += bytes;
    if (sp_session->sendOffset == sp_session->sendQueue.front().size())
    {
        sp_session->queuedSendBytes -= sp_session->sendQueue.front().size();
        // 맨 앞줄의 마지막 바이트까지 송신 완료가 왔다. 여기서야 그 줄을 큐에서 지운다.
        sp_session->sendQueue.pop();
        sp_session->sendOffset = 0;
    }
    // 일부만 보냈으면 같은 줄의 남은 바이트를, 한 줄이 끝났으면 큐의 다음 줄을 보낸다.
    if (!sp_session->sendQueue.empty() && !PostSend(*sp_session))
        CloseSessionLocked(context, *sp_session);
}

void RunIocpWorker(stWorkerContext& context)
{
    const string worker = "[IOCP " + to_string(GetCurrentThreadId()) + "] ";
    Log(worker + "시작");

    while (true)
    {
        DWORD bytes = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* p_overlapped = nullptr;

        // 두 IOCP 쓰레드가 같은 포트에서 기다린다. 완료 한 건은 둘 중 한 쓰레드가 가져간다.
        const BOOL success = GetQueuedCompletionStatus(context.completionPort,
            &bytes, &completionKey, &p_overlapped, INFINITE);
        const DWORD error = success ? ERROR_SUCCESS : GetLastError();

        if (p_overlapped != nullptr)
        {
            // Windows가 넣은 송수신 완료. bytes는 이번에 처리된 바이트 수다.
            HandleSessionCompletion(context, reinterpret_cast<stSession*>(completionKey),
                p_overlapped, bytes, success, error, worker);
            continue;
        }

        if (!success)
        {
            Log("완료 통지 대기 실패. 오류 코드: " + to_string(error));
            terminate();
        }
        if (completionKey == 0)
            break; // StopWorkers가 IOCP 쓰레드 하나당 하나씩 넣어 준 종료 알림.

        // PostNotice가 넣은 p_notice 주소가 completionKey로 돌아왔다. HandleNotice에서 type과 세션을 읽는다.
        unique_ptr<stNotice> up_notice(reinterpret_cast<stNotice*>(completionKey));
        HandleNotice(context, *up_notice);
    }
    Log(worker + "종료");
}

// 목록 잠금은 복사할 때만 잡는다. 다른 접속의 송신 큐를 만질 때는 각 세션의 잠금만 잡는다.
vector<shared_ptr<stSession>> GetSessions(stWorkerContext& context)
{
    vector<shared_ptr<stSession>> sessions;
    lock_guard<mutex> lock(context.sessionsMutex);
    for (const auto& entry : context.sessions)
        sessions.push_back(entry.second);
    return sessions;
}

void BroadcastChat(stWorkerContext& context, const stJob& job)
{
    const string line = "[사용자 " + to_string(job.sp_session->id) + "] " + job.data + "\n";

    // 동시에 두 사람이 보내도 방 안에서 메시지가 같은 순서로 쌓인다.
    lock_guard<mutex> roomLock(context.broadcastMutex);
    for (const auto& sp_target : GetSessions(context))
    {
        // job.sp_session은 글을 쓴 A로 고정된다. sp_target은 접속 목록을 돌면서 A, B, C 중 한 사람을 가리킨다.
        if (sp_target == job.sp_session)
            continue;

        lock_guard<mutex> lock(sp_target->sessionMutex);
        // B 차례에는 B의 송신 큐에, C 차례에는 C의 송신 큐에 같은 줄을 넣는다.
        if (sp_target->inRoom && !sp_target->closing)
            QueueSendLocked(context, sp_target, line);
    }
}

void RunLogicWorker(stWorkerContext& context)
{
    const string worker = "[로직 " + to_string(GetCurrentThreadId()) + "] ";
    Log(worker + "시작");
    stJob job;
    // IOCP 워커가 공용 큐에 넣어 둔 채팅 한 건을 꺼낸다. BroadcastChat이 보낸 사람을 제외한 접속에 배분한다.
    while (context.jobQueue.Pop(job))
    {
        Log(worker + "접속 " + to_string(job.sp_session->id) + " 채팅 배분");
        BroadcastChat(context, job);

        // 다른 사람들의 송신 완료까지 기다릴 필요 없이 보낸 사람의 다음 줄을 처리한다.
        if (!PostNotice(context, Notice::ReceiveNext, job.sp_session))
        {
            lock_guard<mutex> lock(job.sp_session->sessionMutex);
            CloseSessionLocked(context, *job.sp_session);
        }
    }
    Log(worker + "종료");
}

// 메인이 더 이상 접속을 받지 않는 상태에서 호출한다.
void StopWorkers(stWorkerContext& context,
    thread (&iocpWorkers)[IocpWorkerCount],
    thread (&logicWorkers)[LogicWorkerCount])
{
    // 목록 잠금을 풀고 각 세션을 닫는다. 잠금 순서를 뒤집지 않는다.
    for (const auto& sp_session : GetSessions(context))
    {
        lock_guard<mutex> lock(sp_session->sessionMutex);
        CloseSessionLocked(context, *sp_session);
    }

    // 로직 워커가 더 이상 IOCP에 결과를 넣지 않도록 먼저 끝낸다.
    context.jobQueue.Stop();
    for (auto& worker : logicWorkers)
        if (worker.joinable())
            worker.join();

    // 취소된 수신/송신의 완료 통지도 IOCP 워커가 끝까지 받는다.
    {
        unique_lock<mutex> lock(context.sessionsMutex);
        context.sessionsEmpty.wait(lock, [&context] { return context.sessions.empty(); });
    }

    for (auto& worker : iocpWorkers)
    {
        if (worker.joinable() &&
            !PostQueuedCompletionStatus(context.completionPort, 0, 0, nullptr))
        {
            Log("IOCP 워커 종료 알림 등록 실패. 오류 코드: " + to_string(GetLastError()));
            terminate();
        }
    }
    for (auto& worker : iocpWorkers)
        if (worker.joinable())
            worker.join();
}

// 접속을 받을 소켓을 준비한다. 실제 클라이언트 접속은 main의 accept에서 받는다.
bool StartListening(stServerResources& server, const char* p_ip, unsigned short port)
{
    // accept로 받은 클라이언트 소켓도 비동기 송수신을 쓸 수 있게 만든다.
    server.listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
        nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (server.listener == INVALID_SOCKET)
    {
        cerr << "접속 대기 소켓 생성 실패. 오류 코드: " << WSAGetLastError() << '\n';
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    // "127.0.0.1" 같은 문자열을 소켓이 사용하는 IPv4 주소로 바꾼다.
    const int result = InetPtonA(AF_INET, p_ip, &address.sin_addr);
    if (result != 1)
    {
        if (result == 0)
            cerr << "올바른 IPv4 주소가 아닙니다: " << p_ip << '\n';
        else
            cerr << "IPv4 주소 변환 실패. 오류 코드: " << WSAGetLastError() << '\n';
        return false;
    }

    // bind로 이 소켓이 사용할 IP와 포트를 정한다.
    if (bind(server.listener, reinterpret_cast<const sockaddr*>(&address),
        sizeof(address)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        cerr << "소켓에 IP와 포트 지정 실패. 오류 코드: " << error << '\n';
        if (error == WSAEADDRINUSE)
            cerr << p_ip << ':' << port << "이 이미 사용 중입니다. 실행 중인 서버가 있는지 확인하세요.\n";
        return false;
    }

    // listen까지 성공하면 이 주소로 들어오는 접속을 받을 준비가 끝난다.
    if (listen(server.listener, SOMAXCONN) == SOCKET_ERROR)
    {
        cerr << "소켓의 접속 대기 설정 실패. 오류 코드: " << WSAGetLastError() << '\n';
        return false;
    }
    return true;
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    stServerResources server;
    WSADATA wsaData{};
    // 소켓을 사용하기 전에 Winsock부터 초기화한다.
    const int startupError = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupError != 0)
    {
        cerr << "네트워크 초기화 실패. 오류 코드: " << startupError << '\n';
        return 1;
    }
    server.winsockStarted = true;

    constexpr char ListenIp[] = "127.0.0.1";
    constexpr unsigned short ListenPort = 9000;
    if (!StartListening(server, ListenIp, ListenPort))
        return 1;

    // 동시 실행 한도를 2로 설정한다. 스레드는 아래에서 직접 생성한다.
    server.completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
        nullptr, 0, IocpWorkerCount);
    if (server.completionPort == nullptr)
    {
        cerr << "IOCP 생성 실패. 오류 코드: " << GetLastError() << '\n';
        return 1;
    }

    // 메인이 컨텍스트 하나를 만들고 참조로 전달한다. 네 워커가 같은 세션 목록과 작업 큐를 본다.
    stWorkerContext context;
    context.completionPort = server.completionPort;
    thread iocpWorkers[IocpWorkerCount];
    thread logicWorkers[LogicWorkerCount];
    try
    {
        for (auto& worker : iocpWorkers)
            worker = thread(RunIocpWorker, ref(context));
        for (auto& worker : logicWorkers)
            worker = thread(RunLogicWorker, ref(context));
    }
    catch (const system_error& error)
    {
        Log("워커 생성 실패. 오류 코드: " + to_string(error.code().value()));
        StopWorkers(context, iocpWorkers, logicWorkers);
        return 1;
    }

    unsigned int nextSessionId = 1;
    Log("클라이언트 접속 대기 중: " + string(ListenIp) + ":" + to_string(ListenPort));
    try
    {
        while (true)
        {
            auto sp_session = make_shared<stSession>();
            sp_session->socket = accept(server.listener, nullptr, nullptr);
            if (sp_session->socket == INVALID_SOCKET)
            {
                const int error = WSAGetLastError();
                Log("접속 수락 실패. 오류 코드: " + to_string(error));
                if (error == WSAECONNRESET)
                    continue;
                break;
            }
            sp_session->id = nextSessionId++;

            // 이 소켓의 완료 통지를 기존 IOCP로 보낸다. 완료 키는 이 세션을 다시 찾을 때 쓴다.
            if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(sp_session->socket),
                server.completionPort, reinterpret_cast<ULONG_PTR>(sp_session.get()), 0) == nullptr)
            {
                Log("소켓과 IOCP 연결 실패. 오류 코드: " + to_string(GetLastError()));
                continue;
            }

            // 첫 I/O를 등록하기 전에 공용 목록에 세션을 넣는다.
            {
                lock_guard<mutex> lock(context.sessionsMutex);
                context.sessions.emplace(sp_session.get(), sp_session);
            }
            if (!PostNotice(context, Notice::NewSession, sp_session))
            {
                lock_guard<mutex> lock(sp_session->sessionMutex);
                CloseSessionLocked(context, *sp_session);
                break;
            }
        }
    }
    catch (const exception&)
    {
        Log("접속 처리 중 예외가 발생했습니다. 서버를 종료합니다.");
    }

    StopWorkers(context, iocpWorkers, logicWorkers);
    return 1;
}
