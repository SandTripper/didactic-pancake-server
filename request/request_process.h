#ifndef REQUEST_PROCESS_H
#define REQUEST_PROCESS_H

#include <unistd.h>
#include <list>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>

#include "../locker/locker.h"
#include "../sqlconnpool/sql_connection_pool.h"
#include "../threadpool/threadpool.h"
#include "../log/log.h"

class DataPacket;

class requestProcess
{
public:
    //读缓冲区的大小
    static const int READ_BUFFER_SIZE = 1024;
    //写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;

    /*数据包类型
     HBT表示发送心跳包；
     LGN表示登录请求；
     RGT表示注册请求；
     LGT表示登出请求；
     SCU表示查找用户请求；
     ADF表示添加好友请求；
     DEF表示删除好友；
     RFR表示回复好友请求；
     RCN表示重连请求；
     GFI表示获取好友列表请求；
     AFI表示增加好友；
     DFI表示去除好友；
     SMA表示发送消息；
     RMA表示接收消息；
     RDY表示客户端就绪；
     IGN：服务器独有，用于表示发送到一半的数据包
     */
    enum PACKET_TYPE
    {
        HBT = 0,
        LGN,
        RGT,
        LGT,
        SCU,
        ADF,
        DEF,
        RFR,
        RCN,
        GFI,
        AFI,
        DFI,
        SMA,
        RMA,
        RDY,
        IGN
    };

    /*主状态机的三种可能状态
    CHECK_STATE_TYPELINE 表示当前正在分析状态行；
    CHECK_STATE_HEADER 表示当前正在头部字段；
    CHECK_STATE_CONTENT 表示当前正在分析正文*/
    enum CHECK_STATE
    {
        CHECK_STATE_TYPELINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT,
    };

    /*行的读取状态
    LINE_OK 表示读取到一个完整的行；
    LINE_BAD 表示行出错；
    LINE_OPEN 表示行数据尚且不完整*/
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN,
    };

    /*处理数据包的结果：
    NO_REQUEST表示请求不完整，需要继续读取数据包：
    GET_REQUEST表示获得了一个完整的数据包；
    BAD_REQUSET表示数据包有语法错误；
    INTERNAL_ERROR表示内部错误；
    CLOSED_CONNECTION连接断开*/
    enum RESULT_CODE
    {
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

public:
    requestProcess();

    ~requestProcess();

public:
    //初始化新接受的连接
    void init(int sockfd, const sockaddr_in &addr, connectionPool *connPool, int listenfd_Trig_mode, int connfd_Trig_mode);

    //关闭连接,并决定是否登出
    void close_conn(bool wantLogout = false);

    //工作线程函数
    void process(int mode);

    //非阻塞读操作
    bool read();

    //非阻塞写操作
    bool write();

    //返回客户的地址
    sockaddr_in *get_address();

    //初始化数据库，读出到map
    void initmysql_result(connectionPool *connPool);

    //往待发送链表头部添加数据包
    void append_front_data(const DataPacket &data);

    //往待发送链表尾部添加数据包
    void append_back_data(const DataPacket &data);

private:
    //初始化连接
    void init();

    //初始化读
    void init_read();

    //初始化写
    void init_write();

    //解析数据包
    RESULT_CODE process_read();

    //生成数据包
    void process_write();

    //下面这一组函数被process_read调用以解析数据包
    RESULT_CODE parse_type_line(char *text);
    RESULT_CODE parse_headers(char *text);
    RESULT_CODE parse_content(char *text);
    RESULT_CODE do_request();
    char *get_line();
    LINE_STATUS parse_line();

    //下面这一组函数被process_write调用以生成数据包
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(const char *status);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_blank_line();

    //下面这一组为客户请求逻辑处理函数
    //处理心跳包逻辑
    void heartbeat();
    //处理登录逻辑
    void login();
    //处理注册逻辑
    void regis();
    //处理登出逻辑
    void logout();
    //处理搜索用户逻辑
    void search_user();
    //处理添加好友逻辑
    void add_friend();
    //处理删除好友逻辑
    void delete_friend();
    //处理回复好友申请逻辑
    void reply_friend_request();
    //处理重连逻辑
    void reconnect();
    //处理获取好友列表
    void get_friend_items();
    //处理发送消息逻辑
    void send_message();
    //处理客户端就绪逻辑
    void client_ready();

    // REQUEST转const char*
    const char *ReqToString(PACKET_TYPE r);

    //获取毫秒级别时间戳
    long long timestamp();

public:
    /*所有socket上的事件都被注册到同一个epoll内核事件表中,
    所以将epoll文件描述符设置为静态的*/
    static int m_epollfd;

    //统计用户数量
    static int m_user_count;

    //指向全局唯一数据库连接池实例的指针
    connectionPool *m_connPool;

    //线程池对象
    static threadpool *m_threadpool;

    //所有requestProcess对象
    static requestProcess *m_users;

private:
    //该连接的socket和对方的socket地址
    int m_sockfd;
    sockaddr_in m_address;

    //读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];

    //标识读缓冲中已经读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;

    //当前正在分析的字符在读缓冲区中的位置
    int m_checked_idx;

    //当前正在解析的行的起始位置
    int m_start_line;

    //写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];

    //写缓冲区中待发送的字节数
    int m_write_idx;

    //主状态机当前所处的状态
    CHECK_STATE m_check_state;

    //客户的sessionID
    string m_sessionID;

    //数据包类型
    PACKET_TYPE m_method;

    // 数据包正文的长度
    int m_content_length;

    //数据包正文已读取的长度
    int m_content_len_have_read;

    //存储数据包正文
    char *m_content;

    //需要发送的字节数
    int m_bytes_to_send;
    //已经发送的字节数
    int m_bytes_have_send;

    // listenfd是否开启ET模式，ET模式为1，LT模式为0
    int m_listenfd_Trig_mode;

    // connfd是否开启ET模式，ET模式为1，LT模式为0
    int m_connfd_Trig_mode;

    //当前是否有线程在处理写请求,配一把互斥锁
    mutexLocker m_lock_isProcessWrite;
    int m_is_process_write;

    //待发送的数据包,配一把互斥锁
    mutexLocker m_lock_datas;
    list<DataPacket> m_datas;
};

//数据包类
class DataPacket
{
public:
    DataPacket()
    {
        category = requestProcess::HBT;
        content_len = 0;
        content = NULL;
    }
    //传入的字符串以'\0'标识结尾
    DataPacket(requestProcess::PACKET_TYPE cat, const char *con)
    {
        category = cat;
        content_len = strlen(con);
        content = new char[content_len + 1];
        strncpy(content, con, content_len);
        content[content_len] = '\0';
    }
    DataPacket(requestProcess::PACKET_TYPE cat, int conlen, const char *con)
    {
        category = cat;
        content_len = conlen;
        content = new char[conlen + 1];
        strncpy(content, con, conlen);
        content[content_len] = '\0';
    }
    DataPacket(const DataPacket &other)
    {
        category = other.category;
        content_len = other.content_len;
        content = new char[content_len + 1];
        strncpy(content, other.content, content_len);
        content[content_len] = '\0';
    }
    ~DataPacket()
    {
        delete[] content;
    }
    requestProcess::PACKET_TYPE category;
    int content_len;
    char *content;
};

#endif // REQUEST_PROCESS_H
