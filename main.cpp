#include "locker/locker.h"
#include "threadpool/threadpool.h"
#include "request/request_process.h"
#include "./timer/time_wheel_timer.h"
#include "./sqlconnpool/sql_connection_pool.h"
#include "./log/log.h"
#include "config/config.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMELIMIT 30           //超时单位

//这三个函数在request_process.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool oneshot, int triggermode);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

int pipefd[2];

//设置定时器相关参数
static time_wheel time_whl;
static int epollfd = 0;

//信号处理函数
void sig_handler(int sig)
{
    //为保证函数可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    time_whl.tick();
    alarm(time_whl.SI);
}

//定时器回调函数，删除非活动在socket上的注册事件，并关闭
void cb_func(client_data *user)
{
    (requestProcess::m_users + user->sockfd)->close_conn(true);
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
    srand(time(0));

    std::string ipAddress;
    int tcp_port;
    int udp_port;
    int logmode = 0;
    int connfd_Trig_mode;
    int listenfd_Trig_mode;
    int threadnum;

    const char ConfigFile[] = "server.config";
    Config configSettings(ConfigFile);

    tcp_port = configSettings.Read("tcp_port", 0);
    udp_port = configSettings.Read("udp_port", 0);
    ipAddress = configSettings.Read("localhost", ipAddress);

    connfd_Trig_mode = configSettings.Read("connection_mode", 0);
    listenfd_Trig_mode = configSettings.Read("listen_mode", 0);
    logmode = configSettings.Read("log_mode", 0);
    threadnum = configSettings.Read("threadnum", 0);

    if (logmode)
    {
        Log::get_instance()->init("./logs/ServerLog.log", 2000, 800000, 8); //异步日志模型
    }
    else
    {
        Log::get_instance()->init("./logs/ServerLog.log", 2000, 800000, 0); //同步日志模型
    }

    LOG_INFO("ip address:%s", ipAddress.c_str());
    LOG_INFO("tcp port :%d", tcp_port);
    LOG_INFO("udp port :%d", udp_port);
    Log::get_instance()->flush();

    if (listenfd_Trig_mode)
    {
        LOG_INFO("listen mode : ET");
        Log::get_instance()->flush();
    }
    else
    {
        LOG_INFO("listen mode : LT");
        Log::get_instance()->flush();
    }

    if (connfd_Trig_mode)
    {
        LOG_INFO("connection mode : ET");
        Log::get_instance()->flush();
    }
    else
    {
        LOG_INFO("connection mode : LT");
        Log::get_instance()->flush();
    }

    if (logmode)
    {
        LOG_INFO("log mode : Asynchronous");
        Log::get_instance()->flush();
    }
    else
    {
        LOG_INFO("log mode : Synchronous");
        Log::get_instance()->flush();
    }

    //忽略SIGPIPE信号
    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    connectionPool *connPool = connectionPool::get_instance();
    connPool->init("localhost", "root", "", "didactic_pancake", 3306, 8);

    //创建线程池
    threadpool *pool = NULL;
    try
    {
        pool = new threadpool(threadnum);
    }
    catch (...)
    {
        return 1;
    }
    requestProcess::m_threadpool = pool;

    //预先为每个可能的客户连接分配一个requestProcess对象
    requestProcess *users = new requestProcess[MAX_FD];
    assert(users);
    requestProcess::m_users = users;

    //初始化数据库读取表
    users->initmysql_result(connPool);
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int udpSocket = socket(PF_INET, SOCK_DGRAM, 0);
    assert(udpSocket >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ipAddress.c_str(), &address.sin_addr);
    address.sin_port = htons(tcp_port);

    // udp
    struct sockaddr_in udp_address;
    bzero(&udp_address, sizeof(udp_address));
    udp_address.sin_family = AF_INET;
    inet_pton(AF_INET, ipAddress.c_str(), &udp_address.sin_addr);
    udp_address.sin_port = htons(udp_port);

    //设置端口重用
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    ret = bind(udpSocket, (struct sockaddr *)&udp_address, sizeof(udp_address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false, listenfd_Trig_mode);
    requestProcess::m_epollfd = epollfd;

    addfd(epollfd, udpSocket, false, 0);

    //创建信号处理函数用以通知主循环的管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false, listenfd_Trig_mode);

    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false; //记录有没有SIGALRM信号待处理
    alarm(time_whl.SI);

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            //处理客户新到的连接
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                if (connfd_Trig_mode == 0)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        continue;
                    }
                    if (requestProcess::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        continue;
                    }
                    //初始化客户连接
                    users[connfd].init(connfd, client_address, connPool, listenfd_Trig_mode, connfd_Trig_mode);

                    //初始化client_data数据，添加定时器，设置回调函数，绑定用户数据
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    tw_timer *timer = time_whl.add_timer(TIMELIMIT);
                    users_timer[connfd].timer = timer;
                    timer->cb_func = cb_func;
                    timer->user_data = &users_timer[connfd];
                }
                else
                {

                    while (1)
                    {
                        int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                        if (connfd < 0)
                        {
                            LOG_ERROR("%s:errno is:%d", "accept error", errno);
                            break;
                        }
                        if (requestProcess::m_user_count >= MAX_FD)
                        {
                            show_error(connfd, "Internal server busy");
                            LOG_ERROR("%s", "Internal server busy");
                            break;
                        }
                        //初始化客户连接
                        users[connfd].init(connfd, client_address, connPool, listenfd_Trig_mode, connfd_Trig_mode);

                        //初始化client_data数据，添加定时器，设置回调函数，绑定用户数据
                        users_timer[connfd].address = client_address;
                        users_timer[connfd].sockfd = connfd;
                        time_t cur = time(NULL);
                        tw_timer *timer = time_whl.add_timer(TIMELIMIT);
                        users_timer[connfd].timer = timer;
                        timer->cb_func = cb_func;
                        timer->user_data = &users_timer[connfd];
                    }
                    continue;
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                tw_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    time_whl.del_timer(timer);
                }
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);

                if (ret <= 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
            else if (sockfd == udpSocket)
            {
                requestProcess::handleUdpPack(sockfd);
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                tw_timer *timer = users_timer[sockfd].timer;
                //根据读的结果，决定是将任务添加到线程池，还是要关闭连接
                if (users[sockfd].read())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    pool->append(users + sockfd, 0);

                    //有数据传输，将定时器往后延迟
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        time_whl.del_timer(timer);
                        timer = time_whl.add_timer(TIMELIMIT);
                        users_timer[sockfd].timer = timer;
                        timer->cb_func = cb_func;
                        timer->user_data = &users_timer[sockfd];
                    }
                }
                else //关闭连接
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        time_whl.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                tw_timer *timer = users_timer[sockfd].timer;

                //根据写的结果，决定是否关闭连接
                if (users[sockfd].write())
                {

                    //有数据传输，将定时器往后延迟
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        time_whl.del_timer(timer);
                        timer = time_whl.add_timer(TIMELIMIT);
                        users_timer[sockfd].timer = timer;
                        timer->cb_func = cb_func;
                        timer->user_data = &users_timer[sockfd];

                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                    }
                }
                else //关闭连接
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        time_whl.del_timer(timer);
                    }
                }
            }
        }
        //最后处理定时时间
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(udpSocket);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}