#include "JobQueue.h"

bool CJobQueue::Push(stJob job)
{
    {
        lock_guard<mutex> lock(m_jobsMutex);
        if (m_stopping)
            return false;
        // 여기는 방 전체의 작업 큐다. 보낸 사람의 세션과 채팅 본문을 함께 보관한다.
        m_jobs.push(move(job));
    }

    // 먼저 잠금을 풀어야 깨어난 로직 쓰레드가 바로 큐를 확인할 수 있다.
    m_jobsReady.notify_one();
    return true;
}

bool CJobQueue::Pop(stJob& job)
{
    unique_lock<mutex> lock(m_jobsMutex);
    // 큐가 비어 있으면 잠금을 풀고 잔다. 깨어날 때 다시 잠그고 조건을 확인한다.
    m_jobsReady.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
    if (m_jobs.empty())
        return false; // 종료 요청을 받았고 남은 작업도 없다.

    // 일감을 꺼낸 로직 워커가 이 job을 들고 RunLogicWorker로 돌아가 BroadcastChat을 호출한다.
    job = move(m_jobs.front());
    m_jobs.pop();
    // 여기서 반환하면 잠금이 풀린다. 두 로직 쓰레드는 꺼낸 작업을 각자 처리한다.
    return true;
}

void CJobQueue::Stop()
{
    {
        lock_guard<mutex> lock(m_jobsMutex);
        m_stopping = true;
    }
    // 일을 기다리던 로직 쓰레드 모두가 Pop에서 종료 조건을 확인하게 한다.
    m_jobsReady.notify_all();
}
