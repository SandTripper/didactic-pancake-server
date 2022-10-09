#ifndef REQUEST_PROCESS_H
#define REQUEST_PROCESS_H

#include <unistd.h>
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
#include "../log/log.h"

class requestProcess
{
public:
    //读缓冲区的大小
    static const int READ_BUFFER_SIZE = 1024;
    //写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;

    /*请求类型
    HBT表示发送心跳包；
    LGN表示登录请求；
    RGT表示注册请求；
    LGT表示登出请求；
    */
    enum REQUEST
    {
        HBT = 0,
        LGN,
        RGT,
        LGT
    };

    //主状态机的两种可能状态，分别表示：当前正在分析请求行，当前正在分析内容
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT,
    };

    //行的读取状态，分别表示：读取到一个完整的行，行出错，行数据尚且不完整
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN,
    };

    /*服务器处理请求的结果：
    NO_REQUEST表示请求不完整，需要继续读取客户数据：
    GET_REQUEST表示获得了一个完整的的客户请求；
    BAD_REQUSET表示客户请求有语法错误；
    INTERNAL_ERROR表示服务器内部错误；
    CLOSED_CONNECTION表示客户端已经关闭连接*/
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
    //初始化新接受的链接
    void init(int sockfd, const sockaddr_in &addr, connectionPool *connPool, int listenfd_Trig_mode, int connfd_Trig_mode);

    //关闭连接
    void close_conn(bool real_close = true);

    //处理客户端请求
    void process();

    //非阻塞读操作
    bool read();

    //非阻塞写操作
    bool write();

    //返回客户的地址
    sockaddr_in *get_address();

    //初始化数据库，读出到map
    void initmysql_result(connectionPool *connPool);

private:
    //初始化连接
    void init();

    //解析请求
    RESULT_CODE process_read();

    //填充应答
    bool process_write(RESULT_CODE ret);

    //下面这一组函数被process_read调用以分析请求
    RESULT_CODE parse_request_line(char *text);
    RESULT_CODE parse_headers(char *text);
    RESULT_CODE parse_content(char *text);
    RESULT_CODE do_request();
    char *get_line();
    LINE_STATUS parse_line();

    //下面这一组函数被process_write调用以填充HTTP应答
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(const char *status);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_blank_line();

    //下面这一组为客户请求逻辑处理函数
    //处理登录逻辑
    void login();
    //处理注册逻辑
    void regis();
    //处理登出逻辑
    void logout();

public:
    /*所有socket上的事件都被注册到同一个epoll内核事件表中,
    所以将epoll文件描述符设置为静态的*/
    static int m_epollfd;

    //统计用户数量
    static int m_user_count;

    //指向全局唯一连接池实例的指针
    connectionPool *m_connPool;

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
    long long m_sessionID;

    //请求方法
    REQUEST m_method;

    // 请求的消息体的长度
    int m_content_length;

    //需要发送的字节数
    int m_bytes_to_send;
    //已经发送的字节数
    int m_bytes_have_send;

    //存储请求头数据
    char *m_string;

    // listenfd是否开启ET模式，ET模式为1，LT模式为0
    int m_listenfd_Trig_mode;

    // connfd是否开启ET模式，ET模式为1，LT模式为0
    int m_connfd_Trig_mode;

    //返回的状态
    char m_response_status[16];

    //返回的正文长度
    int m_response_content_len;

    //返回的正文
    char m_response_content[1024];
};

#endif // REQUEST_PROCESS_H
