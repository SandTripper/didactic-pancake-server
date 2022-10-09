#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

//封装信号量的类

class sem
{
public:
    //创建并初始化信号量
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            //构造函数没有返回值，通过抛出异常报告错误
            throw std::exception();
        }
    }
    //创建拥有num个生产资料的信号量
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            //构造函数没有返回值，通过抛出异常报告错误
            throw std::exception();
        }
    }
    //销毁信号量
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    //等待信号量
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }
    //增加信号量
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

//封装互斥锁的类

class mutexLocker
{
public:
    //创建并初始化互斥锁
    mutexLocker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    //销毁互斥锁
    ~mutexLocker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    //获取互斥锁
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    //释放互斥锁
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    //获取互斥锁地址
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

//封装条件变量的类
class cond
{
public:
    //创建并初始化条件变量
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }
    //销毁条件变量
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    //等待条件变量
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }
    //等待条件变量,可设置超时时间
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }
    //唤醒等待条件变量的至少一条线程
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    //唤醒等待条件变量的所有线程
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};

//封装读写锁的类

class readWriteLocker
{
public:
    //初始化读写锁
    readWriteLocker()
    {
        readCnt = 0;
        if (pthread_mutex_init(&m_read_mutex, NULL) != 0)
        {
            throw std::exception();
        }
        if (pthread_mutex_init(&m_write_mutex, NULL) != 0)
        {
            pthread_mutex_destroy(&m_read_mutex);
            throw std::exception();
        }
    }

    //销毁读写锁
    ~readWriteLocker()
    {
        pthread_mutex_destroy(&m_read_mutex);
        pthread_mutex_destroy(&m_write_mutex);
    }

    //获取读锁
    bool readLock()
    {
        pthread_mutex_lock(&m_read_mutex);
        if (++readCnt == 1) //保证只加一次写锁
        {
            pthread_mutex_lock(&m_write_mutex);
        }
        pthread_mutex_unlock(&m_read_mutex);
    }

    //释放读锁
    void readUnlock()
    {
        pthread_mutex_lock(&m_read_mutex);
        if (--readCnt == 0)
        {
            pthread_mutex_unlock(&m_write_mutex);
        }
        pthread_mutex_unlock(&m_read_mutex);
    }

    //获取写锁
    bool writeLock()
    {
        pthread_mutex_lock(&m_write_mutex);
    }

    //释放写锁
    void writeUnlock()
    {
        pthread_mutex_unlock(&m_write_mutex);
    }

private:
    pthread_mutex_t m_read_mutex;

    pthread_mutex_t m_write_mutex;

    int readCnt; //已加读锁个数
};

#endif // LOCKER_H