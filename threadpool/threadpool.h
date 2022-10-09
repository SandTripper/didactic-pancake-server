#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <exception>
#include <cstdio>
#include <pthread.h>
#include "../locker/locker.h"
#include "../log/log.h"
#include "../request/request_process.h"

class threadpool
{
public:
    /*参数thread_number是线程池中线程的数量，
    max_request是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_request = 10000);

    ~threadpool();

    //往请求队列中添加任务
    bool append(requestProcess *request);

private:
    //工作线程运行的函数，它不断从工作队列中取出任务并执行
    static void *worker(void *arg);

    void run();

private:
    //线程池中的线程数
    int m_thread_number;
    //请求队列中允许的最大请求数
    int m_max_requests;
    //描述线程池的数组，其大小为m_thread_number
    pthread_t *m_threads;
    //请求队列
    std::list<requestProcess *> m_workqueue;
    //保护请求队列的互斥锁
    mutexLocker m_queuelocker;
    //是否有任务需要处理
    sem m_queuestat;
    //是否结束线程
    bool m_stop;
};

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

bool threadpool::append(requestProcess *request)
{
    //操作工作队列时一定要加锁
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock(); //退出函数前记得解锁
        return false;
    }
    m_workqueue.push_back(request);
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
        requestProcess *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }
        request->process();
    }
}

#endif