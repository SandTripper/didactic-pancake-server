#include "threadpool.h"

#include "../request/request_process.h"

threadpool::threadpool(int thread_number, int max_request)
    : m_thread_number(thread_number), m_max_requests(max_request)
{
    if ((thread_number <= 0) || (max_request <= 0))
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        throw std::exception();
    }

    //创建thread_number个线程，并将它们都设置为脱离线程
    for (int i = 0; i < thread_number; ++i)
    {
        LOG_INFO("create the %dth thread", i);
        Log::get_instance()->flush();
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads; //构造函数出错，在退出前释放已申请成功的资源
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads; //构造函数出错，在退出前释放已申请成功的资源
            throw std::exception();
        }
    }
}

threadpool::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

bool threadpool::append(requestProcess *request, int mode)
{
    //操作工作队列时一定要加锁
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock(); //退出函数前记得解锁
        return false;
    }

    LOG_INFO("append a work mode = %d", mode);
    Log::get_instance()->flush();

    m_workqueue.emplace_back(pair<requestProcess *, int>(request, mode));
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

void *threadpool::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

void threadpool::run()
{
    while (!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        requestProcess *request = m_workqueue.front().first;
        int mode = m_workqueue.front().second;
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }
        request->process(mode);
    }
}