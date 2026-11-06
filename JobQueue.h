#pragma once

#include "Common.h"

struct stSession;

// 로직 쓰레드에 넘길 채팅 한 줄과 보낸 사람.
struct stJob
{
    shared_ptr<stSession> sp_session; // 큐에서 기다리는 동안에도 이 접속의 세션을 유지한다.
    string data;                      // 줄바꿈을 제외한 채팅 본문.
};

// IOCP 쓰레드 두 개가 Push하고 로직 쓰레드 두 개가 Pop하는 공용 작업 큐.
class CJobQueue
{
public:
    bool Push(stJob job);
    bool Pop(stJob& job);
    void Stop();

private:
    queue<stJob> m_jobs;            // 먼저 들어온 작업부터 꺼낸다.
    mutex m_jobsMutex;              // m_jobs와 m_stopping을 함께 보호한다.
    condition_variable m_jobsReady; // 작업 추가나 종료를 기다리는 로직 쓰레드를 깨운다.
    bool m_stopping = false;        // 종료 요청 후에는 이미 들어온 작업까지만 꺼낸다.
};
