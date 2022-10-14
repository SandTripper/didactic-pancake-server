#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <exception>
#include <cstdio>
#include <pthread.h>
#include "../locker/locker.h"
#include "../log/log.h"

class requestProcess;

class threadpool
{
public:
    /*参数thread_number是线程池中线程的数量，
    max_request是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_request = 10000);

    ~threadpool();

    //往请求队列中添加任务
    bool append(requestProcess *request, int mode);

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
    std::list<pair<requestProcess *, int>> m_workqueue;
    //保护请求队列的互斥锁
    mutexLocker m_queuelocker;
    //是否有任务需要处理
    sem m_queuestat;
    //是否结束线程
    bool m_stop;
};

#endif