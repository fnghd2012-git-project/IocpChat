#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using std::atomic_bool;
using std::cerr;
using std::char_traits;
using std::cin;
using std::condition_variable;
using std::cout;
using std::endl;
using std::exception;
using std::flush;
using std::getline;
using std::istringstream;
using std::lock_guard;
using std::make_shared;
using std::memcpy;
using std::move;
using std::mutex;
using std::queue;
using std::ref;
using std::shared_ptr;
using std::size_t;
using std::string;
using std::system_error;
using std::terminate;
using std::thread;
using std::to_string;
using std::to_wstring;
using std::unique_lock;
using std::unique_ptr;
using std::unordered_map;
using std::vector;
using std::ws;
using std::wstring;

// 클라이언트는 한 줄씩 보낸다. 서버가 붙이는 사용자 번호는 본문 제한에 포함하지 않는다.
constexpr size_t MaxChatBytes = 4096;
constexpr size_t MaxServerLineBytes = MaxChatBytes + 32;

enum class LineResult
{
    Ready,
    Incomplete,
    TooLong
};

// TCP는 한 줄을 나눠 주거나 여러 줄을 한꺼번에 줄 수 있다. 덜 온 부분은 pending에 남긴다.
inline LineResult TakeChatLine(string& pending, size_t maxBytes, string& line)
{
    const size_t end = pending.find('\n');
    if (end == string::npos)
    {
        // CRLF가 둘로 나뉘어 왔을 때 마지막 CR은 본문 길이에 포함하지 않는다.
        const size_t size = !pending.empty() && pending.back() == '\r' ? pending.size() - 1 : pending.size();
        return size <= maxBytes ? LineResult::Incomplete : LineResult::TooLong;
    }
    const size_t size = end > 0 && pending[end - 1] == '\r' ? end - 1 : end;
    if (size > maxBytes)
        return LineResult::TooLong;

    line.assign(pending, 0, size);
    pending.erase(0, end + 1);
    return LineResult::Ready;
}
