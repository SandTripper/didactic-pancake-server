#include "time_wheel_timer.h"

tw_timer::tw_timer(int rot, int ts)
    : next(NULL), prev(NULL), rotation(rot), time_slot(ts)
{
}

time_wheel::time_wheel() : cur_slot(0), time_have_run(0)
{
    start_time = (int)time(NULL);
    slots = new tw_timer *[N];
    for (int i = 0; i < N; ++i)
    {
        slots[i] = NULL; //初始化每个槽的头节点
    }
}

time_wheel::~time_wheel()
{
    //遍历槽，销毁其中的定时器
    for (int i = 0; i < N; ++i)
    {
        tw_timer *tmp = slots[i];
        while (tmp)
        {
            slots[i] = tmp->next;
            delete tmp;
            tmp = slots[i];
        }
    }
    delete[] slots;
}

tw_timer *time_wheel::add_timer(int timeout)
{

    if (timeout < 0)
    {
        return NULL;
    }

    int ticks = 0; //记录还要转多少次

    //如果timeout不满一个间隔SI，则向上取整为1，否则向下取整

    if (timeout < SI)
    {
        ticks = 1;
    }
    else
    {
        ticks = timeout / SI;
    }

    //计算待插入的定时器转动多少圈后触发
    int rot = ticks / N;

    //计算待插入的定时器应该存放在哪个槽
    int ts = (cur_slot + (ticks % N)) % N;

    //创建新的定时器
    tw_timer *timer = new tw_timer(rot, ts);

    //如果第ts个槽没有定时器，则设置timer为该槽的头节点
    if (!slots[ts])
    {
        slots[ts] = timer;
    }
    //否则将timer插入第ts槽的链表
    else
    {
        timer->next = slots[ts];
        slots[ts]->prev = timer;
        slots[ts] = timer;
    }

    return timer;
}

void time_wheel::del_timer(tw_timer *timer)
{

    if (!timer)
    {
        return;
    }

    int ts = timer->time_slot;

    //如果目标定时器就是第ts槽的头节点，则重新设置头节点后再删除
    if (slots[ts] == timer)
    {
        slots[ts] = timer->next;
        if (slots[ts])
        {
            slots[ts]->prev = NULL;
        }
        delete timer;
    }
    //否则直接连接timer在链表的前后节点后再删除
    else
    {
        timer->prev->next = timer->next;
        if (timer->next)
        {
            timer->next->prev = timer->prev;
        }
        delete timer;
    }
}

void time_wheel::tick()
{
    int cur = (int)time(NULL);

    while (time_have_run <= cur - start_time)
    {
        tw_timer *tmp = slots[cur_slot]; //取得当前槽的头节点

        while (tmp) //遍历链表
        {
            //如果当前遍历的定时器的rotation的值大于0，说明还没到定的时间
            if (tmp->rotation > 0)
            {
                tmp->rotation--;
                tmp = tmp->next;
            }
            //否则，说明定时器到期，执行定时任务，然后删除该定时器
            else
            {
                tmp->cb_func(tmp->user_data);
                //如果tmp就是该槽的头节点，则重新设置头节点后再删除
                if (tmp == slots[cur_slot])
                {
                    slots[cur_slot] = tmp->next;
                    delete tmp;
                    if (slots[cur_slot])
                    {
                        slots[cur_slot]->prev = NULL;
                    }
                    tmp = slots[cur_slot];
                }
                //否则直接连接tmp在链表的前后节点后再删除
                else
                {
                    tmp->prev->next = tmp->next;
                    if (tmp->next)
                    {
                        tmp->next->prev = tmp->prev;
                    }
                    tw_timer *tmp2 = tmp->next;
                    delete tmp;
                    tmp = tmp2;
                }
            }
        }
        cur_slot = (cur_slot + 1) % N; //更新当前的槽
        time_have_run += SI;
    }
}
