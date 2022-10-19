#ifndef TIME_WHEEL_TIMER_H
#define TIME_WHEEL_TIMER_H

#include <time.h>
#include <netinet/in.h>
#include <cstdio>

class tw_timer; //前向声明

//绑定socket和定时器
struct client_data
{
    sockaddr_in address;
    int sockfd;
    tw_timer *timer;
};

//定时器类

class tw_timer
{
public:
    tw_timer(int rot, int ts);

public:
    //记录定时器在时间轮转多少圈之后生效
    int rotation;

    //记录定时器属于时间轮的哪个槽
    int time_slot;

    //回调函数
    void (*cb_func)(client_data *);

    //客户数据
    client_data *user_data;

    //指向下一个定时器
    tw_timer *next;

    //指向前一个定时器
    tw_timer *prev;
};

//时间轮类

class time_wheel
{

public:
    //时间轮上槽的数目
    static const int N = 60;

    //转动一次经过的时间
    static const int SI = 1;

    //记录定时器启动时的时间戳
    int start_time;

    //记录定时器运行的时间数
    int time_have_run;

public:
    time_wheel();

    ~time_wheel();

    //根据定时值timeout创建一个新的定时器，把他插入合适的槽中，并返回指向定时器内存的指针
    tw_timer *add_timer(int timeout);

    //删除目标定时器timer
    void del_timer(tw_timer *timer);

    // SI时间到后，调用该函数，时间轮向前转一下
    void tick();

private:
    //时间轮的槽，每个元素指向一个定时器链表
    tw_timer **slots;

    //时间轮的当前槽
    int cur_slot;
};

#endif