#include "request_process.h"

#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

using namespace std;

int requestProcess::m_user_count = 0;
int requestProcess::m_epollfd = -1;

extern int pipefd[2];

//初始化静态变量
threadpool *requestProcess::m_threadpool = NULL;
requestProcess *requestProcess::m_users = NULL;

//将表中的用户名和密码放入map
map<string, string> user_mp;

/* 储存sessionID对应的用户名
 <sessionID,tar_user>*/
map<string, string> sessionID_mp;
/*储存用户名对应的描述符
 <tar_user,connfd>*/
map<string, int> userfd_mp;
/*储存好友申请
<tar_user,applys>*/
map<string, set<string>> frireq_mp;
/* 储存好友关系
<tar_user,friends>*/
map<string, set<string>> friend_mp;

mutexLocker lock;       //互斥锁
readWriteLocker rwlock; //读写锁

//将文件描述符设置为非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option; //返回原来的状态以便恢复
}

//将内核事件表注册读事件，选择开启EPOLLONESHOT,ET模式
void addfd(int epollfd, int fd, bool oneshot, int triggermode)
{
    epoll_event event;
    event.data.fd = fd;
    if (triggermode)
    {
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    }
    else
    {
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (oneshot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//删除事件
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int triggermode)
{
    epoll_event event;
    event.data.fd = fd;
    if (triggermode)
    {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;
    }
    else
    {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    }

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

requestProcess::requestProcess()
{
}

requestProcess::~requestProcess()
{
}

void requestProcess::init(int sockfd, const sockaddr_in &addr, connectionPool *connpool,
                          int listenfd_Trig_mode, int connfd_Trig_mode)
{
    m_sockfd = sockfd;
    m_address = addr;
    m_connPool = connpool;
    m_listenfd_Trig_mode = listenfd_Trig_mode;
    m_connfd_Trig_mode = connfd_Trig_mode;
    addfd(m_epollfd, sockfd, true, m_connfd_Trig_mode);
    m_user_count++;
    m_sessionID = "";
    m_is_process_write = false;
    init();
}

void requestProcess::init()
{
    init_read();
    init_write();
}

//初始化读
void requestProcess::init_read()
{
    m_check_state = CHECK_STATE_TYPELINE;
    m_method = HBT;
    m_start_line = 0;
    m_checked_idx = 0;
    m_content_length = 0;
    m_read_idx = 0;
    m_content_len_have_read = 0;
    m_content = nullptr;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
}

//初始化写
void requestProcess::init_write()
{
}

void requestProcess::initmysql_result(connectionPool *connPool)
{
    //从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据
    if (mysql_query(mysql, "SELECT user_name,user_password FROM user_tb"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        user_mp[temp1] = temp2;
    }

    //在friend_tb表中检索usera_name，userb_name数据
    if (mysql_query(mysql, "SELECT usera_name,userb_name FROM friend_tb"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    result = mysql_store_result(mysql);

    //返回结果集中的列数
    num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        friend_mp[temp1].insert(temp2);
        friend_mp[temp2].insert(temp1);
    }

    //在sessionID_tb表中检索sessionID，user_name数据
    if (mysql_query(mysql, "SELECT sessionID,user_name FROM sessionID_tb"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    result = mysql_store_result(mysql);

    //返回结果集中的列数
    num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        sessionID_mp[temp1] = temp2;
    }
}

//关闭连接,并决定是否登出
void requestProcess::close_conn(bool wantLogout)
{
    if (m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; //关闭一个连接时，将客户总量减1
        if (wantLogout)
        {
            logout();
        }
        LOG_INFO("close fd %d", m_epollfd);
        Log::get_instance()->flush();
    }
}

void requestProcess::process(int mode)
{
    if (mode == 0) //读操作
    {
        RESULT_CODE read_ret;
        while (m_checked_idx < m_read_idx)
        {
            read_ret = process_read();
            if (read_ret == NO_REQUEST)
            {
                //最后一个数据包不完整，保留残缺的数据行，删除已读过的读缓冲区字符
                int broken_line_len = m_read_idx - m_start_line;
                for (int p1 = 0, p2 = m_start_line; p1 < broken_line_len; ++p1, ++p2)
                {
                    m_read_buf[p1] = p2;
                }
                memset(m_read_buf + broken_line_len, '\0', READ_BUFFER_SIZE - broken_line_len);
                m_read_idx -= m_start_line;
                m_checked_idx = m_read_idx;
                m_start_line = 0;
                break;
            }
            m_check_state = CHECK_STATE_TYPELINE;
            m_start_line = m_checked_idx;
            m_content_len_have_read = 0;
            if (m_content != nullptr)
            {
                delete[] m_content;
                m_content = nullptr;
            }
        }
        if (read_ret != NO_REQUEST)
        {
            init_read();
        }
        else
        {
            // char buf[5] = {MY_SIGREAD};
            // int sofd = m_sockfd;
            // for (int i = 1; i <= 4; i++)
            // {
            //     buf[i] = sofd % 256 - 128;
            //     sofd /= 256;
            // }
            // send(pipefd[1], buf, 5, 0);
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_connfd_Trig_mode);
        }
    }
    else
    {
        process_write();
    }
}

requestProcess::RESULT_CODE requestProcess::process_read()
{
    LINE_STATUS linestatus = LINE_OK; //记录当前行的读取状态
    RESULT_CODE retcode = NO_REQUEST; //记录请求的处理结果
    char *text = nullptr;

    //主状态机，用于从buffer中取出所有完整的行
    while (((m_check_state == CHECK_STATE_CONTENT) && (linestatus == LINE_OK)) ||
           ((linestatus = parse_line()) == LINE_OK))
    {
        text = get_line();            // start_line是行在buffer中 的起始位置
        m_start_line = m_checked_idx; //记录下一行的起始位置

        switch (m_check_state)
        {
        case CHECK_STATE_TYPELINE: //第一个状态，分析数据包类型行
            retcode = parse_type_line(text);
            if (retcode == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        case CHECK_STATE_HEADER: //第二个状态，分析头部字段
            retcode = parse_headers(text);
            if (retcode == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (retcode == GET_REQUEST)
            {
                return do_request();
            }
            break;
        case CHECK_STATE_CONTENT: //第三个状态，分析数据包正文
            retcode = parse_content(text);
            if (retcode == GET_REQUEST)
            {
                return do_request();
            }
            linestatus = LINE_OPEN;
            break;
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

bool requestProcess::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return true;
    }
    int bytes_read = 0;

    if (m_connfd_Trig_mode) // ET模式，需要循环读取完所有数据
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                              READ_BUFFER_SIZE - m_read_idx, 0);

            if (bytes_read == -1)
            {
                /* EAGAIN:在非阻塞模式下调用了阻塞操作即读操作，
                不需要管，此外，在VxWorks和Windows上，EAGAIN的名字叫做EWOULDBLOCK*/
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                else
                { //返回值小于0，说明recv在copy时出错，断开连接
                    return false;
                }
            }
            else if (bytes_read == 0) //返回值等于0，说明客户端断开连接
            {
                return false;
            }
            LOG_INFO("get datas:\n%s", m_read_buf + m_read_idx - bytes_read);
            Log::get_instance()->flush();
            m_read_idx += bytes_read;
        }
        return true;
    }
    else // LT模式，直接读取数据
    {

        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0) //返回值小于0，说明recv在copy时出错，返回值等于0，说明客户端断开连接
        {
            return false;
        }
        else
        {
            LOG_INFO("get datas:\n%s", m_read_buf + m_read_idx - bytes_read);
            Log::get_instance()->flush();
            return true;
        }
    }
}

requestProcess::LINE_STATUS requestProcess::parse_line()
{
    char temp;
    /*buffer中第0~checked_index字节都已分析完毕，
    第checked_index~(read_index-1)字节由下面的循环挨个分析*/
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //获得当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前的字节是'\r'，即回车符，则说明可能读取到一个完整的行
        if (temp == '\r')
        {
            /*如果'\r'字符碰巧是目前buffer中最后一个已经被读入的客户数据，
            那么这次分析没有读取到一个完整的行，返回LINE_OPEN以表示还需要
            继续读取客户数据才能进一步分析*/
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            //如果下一个字符是'\n'，则说明我们成功读取到一个完整的行
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则，就说明客户发送的数据包存在语法问题
            return LINE_BAD;
        }
        //如果当前的字节是'\n'，即换行符，则也说明可能读取到一个完整的行
        else if (temp == '\n')
        {
            //如果上一个字符是'\r'，则说明我们成功读取到一个完整的行
            if ((m_checked_idx > 1) && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则，就说明客户发送的数据包存在语法问题
            else
            {
                return LINE_BAD;
            }
        }
    }
    return LINE_OPEN;
}

char *requestProcess::get_line()
{
    return m_read_buf + m_start_line;
}

requestProcess::RESULT_CODE requestProcess::parse_type_line(char *text)
{
    char *method = text;
    if (strcasecmp(method, "HBT") == 0)
    {
        m_method = HBT;
    }
    else if (strcasecmp(method, "LGN") == 0)
    {
        m_method = LGN;
    }
    else if (strcasecmp(method, "RGT") == 0)
    {
        m_method = RGT;
    }
    else if (strcasecmp(method, "LGT") == 0)
    {
        m_method = LGT;
    }
    else if (strcasecmp(method, "SCU") == 0)
    {
        m_method = SCU;
    }
    else if (strcasecmp(method, "ADF") == 0)
    {
        m_method = ADF;
    }
    else if (strcasecmp(method, "DEF") == 0)
    {
        m_method = DEF;
    }
    else if (strcasecmp(method, "RFR") == 0)
    {
        m_method = RFR;
    }
    else if (strcasecmp(method, "RCN") == 0)
    {
        m_method = RCN;
    }
    else if (strcasecmp(method, "GFI") == 0)
    {
        m_method = GFI;
    }
    else if (strcasecmp(method, "AFI") == 0)
    {
        m_method = AFI;
    }
    else if (strcasecmp(method, "DFI") == 0)
    {
        m_method = DFI;
    }
    else if (strcasecmp(method, "SMA") == 0)
    {
        m_method = SMA;
    }
    else if (strcasecmp(method, "RMA") == 0)
    {
        m_method = RMA;
    }
    else if (strcasecmp(method, "RDY") == 0)
    {
        m_method = RDY;
    }
    else if (strcasecmp(method, "SAV") == 0)
    {
        m_method = SAV;
    }
    else if (strcasecmp(method, "RAV") == 0)
    {
        m_method = RAV;
    }
    else
    {
        return BAD_REQUEST;
    }
    //请求行处理完毕，状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

requestProcess::RESULT_CODE requestProcess::parse_headers(char *text)
{
    Log::get_instance()->flush();
    //遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        /*如果数据包有消息体，则还需要读取m_content_length字节的消息体，
        状态机转移到CHECK_STATE_CONTENT状态*/
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们得到了一个完整的请求
        else
        {
            return GET_REQUEST;
        }
    }
    //处理"Content-Length"头部字段
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else //其他头部字段都不处理
    {
        LOG_INFO("unknow header: %s", text);
        Log::get_instance()->flush();
    }

    return NO_REQUEST;
}

requestProcess::RESULT_CODE requestProcess::parse_content(char *text)
{

    if (m_content == nullptr)
    {
        m_content = new char[m_content_length + 1];
    }
    if (m_read_idx >= (m_content_length - m_content_len_have_read + m_checked_idx))
    {
        /*如果当前已读到数据包正文的结尾
        将数据拷贝至m_content并加上'\0'*/

        LOG_INFO("parse %d gooddata,last %d datas", m_read_idx - m_checked_idx, m_content_length - m_content_len_have_read);
        Log::get_instance()->flush();

        memcpy(m_content + m_content_len_have_read, text, m_content_length - m_content_len_have_read);
        m_content[m_content_length] = '\0';
        m_checked_idx += m_content_length - m_content_len_have_read;
        m_start_line = m_checked_idx;
        m_content_len_have_read = m_content_length;
        return GET_REQUEST;
    }
    else
    {
        /*如果当前未读到数据包正文的结尾
        将读到的数据段拷贝至m_content并返回NO_REQUEST等待下次数据*/

        LOG_INFO("parse %d data,last %d datas", m_read_idx - m_checked_idx, m_content_length - m_content_len_have_read);
        Log::get_instance()->flush();

        memcpy(m_content + m_content_len_have_read, text, m_read_idx - m_checked_idx);
        m_content_len_have_read += m_read_idx - m_checked_idx;
        m_start_line = m_read_idx;
        m_checked_idx = m_read_idx;
        return NO_REQUEST;
    }
}

requestProcess::RESULT_CODE requestProcess::do_request()
{
    switch (m_method)
    {
    case HBT:
        heartbeat();
        break;
    case LGN:
        login();
        break;
    case RGT:
        regis();
        break;
    case LGT:
        logout();
        break;
    case SCU:
        search_user();
        break;
    case ADF:
        add_friend();
        break;
    case DEF:
        delete_friend();
        break;
    case RFR:
        reply_friend_request();
        break;
    case RCN:
        reconnect();
        break;
    case GFI:
        get_friend_items();
        break;
    case AFI:
        break;
    case DFI:
        break;
    case SMA:
        send_message();
        break;
    case RMA:
        break;
    case RDY:
        client_ready();
        break;
    case SAV:
        change_avatar();
    default:
        break;
    }
    return GET_REQUEST;
}

void requestProcess::process_write()
{
    m_lock_isProcessWrite.lock();
    if (m_is_process_write) //如果有线程在处理
    {
        m_threadpool->append(this, 1); //重新扔回线程池队列
        m_lock_isProcessWrite.unlock();
        return;
    }
    m_is_process_write = true;
    m_lock_isProcessWrite.unlock();

    m_lock_datas.lock(); //操作数据链表记得加锁
    if (!m_datas.empty())
    {
        LOG_INFO("process_write:EPOLLOUT");
        Log::get_instance()->flush();
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_connfd_Trig_mode);
    }
    m_lock_datas.unlock();

    m_lock_isProcessWrite.lock();
    m_is_process_write = false;
    m_lock_isProcessWrite.unlock();
}

bool requestProcess::write()
{
    int temp = 0;

    while (1)
    {
        m_lock_datas.lock();
        if (m_datas.empty())
        {
            m_lock_datas.unlock();
            break;
        }
        auto pack = m_datas.begin();
        m_lock_datas.unlock();

        temp = send(m_sockfd, pack->m_format_string + pack->byte_have_write, pack->m_format_string_len - pack->byte_have_write, 0);
        if (temp < 0)
        {
            /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，*/
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_connfd_Trig_mode);
                return true;
            }
            else
            {
                return false;
            }
        }
        LOG_INFO("send %d bytes data to the client(%s):\n%s", temp, inet_ntoa(m_address.sin_addr), pack->m_format_string + pack->byte_have_write);
        Log::get_instance()->flush();
        pack->byte_have_write += temp;

        if (pack->m_format_string_len - pack->byte_have_write <= 0)
        {
            //发送数据包成功
            m_lock_datas.lock();
            m_datas.erase(pack);
            m_lock_datas.unlock();
        }
    }
    modfd(m_epollfd, m_sockfd, EPOLLIN, m_connfd_Trig_mode);
    init_write();
    return true;
}

sockaddr_in *requestProcess::get_address()
{
    return &m_address;
}

void requestProcess::append_front_data(const W_DataPacket &data)
{
    m_lock_datas.lock();
    m_datas.emplace_front(data);
    m_lock_datas.unlock();
}

void requestProcess::append_back_data(const W_DataPacket &data)
{
    m_lock_datas.lock();
    m_datas.emplace_back(data);
    m_lock_datas.unlock();
}

void requestProcess::heartbeat()
{
    //返回心跳包原文
    append_back_data(W_DataPacket(HBT, m_content_length, m_content));
    m_threadpool->append(this, 1);
}

void requestProcess::login()
{
    int l = m_content_length;

    string tar_user = "";
    string password = "";

    //如果超出36个字符，说明不合法
    if (l > 36)
    {
        append_back_data(W_DataPacket(LGN, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    //解析用户名和密码
    int lineCnt = 0;
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            lineCnt++;
            i++;
            continue;
        }
        switch (lineCnt)
        {
        case 0:
            tar_user += m_content[i];
            break;
        case 1:
            password += m_content[i];
            break;
        default:
            break;
        }
    }

    //判断用户名和密码是否合法

    bool isNice = true;

    if (tar_user.length() > 16 || tar_user == "" || password.length() > 16 || password == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : tar_user)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
        for (const auto &c : password)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        append_back_data(W_DataPacket(LGN, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    lock.lock();

    auto it = user_mp.find(tar_user);

    if (it == user_mp.end() || it->second != password) //用户名或密码错误
    {
        append_back_data(W_DataPacket(LGN, 3, "0\r\n"));
        m_threadpool->append(this, 1);
    }
    else if (userfd_mp.find(tar_user) != userfd_mp.end()) //用户在线
    {
        append_back_data(W_DataPacket(LGN, 4, "-1\r\n"));
        m_threadpool->append(this, 1);
    }
    else
    {
        //生成sessionID
        long long cur_time = time(0);
        m_sessionID = to_string(cur_time * 1000000 + cur_time % rand() % 1000000);

        sessionID_mp[m_sessionID] = tar_user;

        string sql_insert = "INSERT INTO sessionID_tb(sessionID, user_name) VALUES('" + m_sessionID + "', '" + tar_user + "')";
        //从连接池中取一个连接
        MYSQL *mysql;
        connectionRAII mysqlcon(&mysql, m_connPool);
        LOG_INFO("do mysql query:%s", sql_insert.c_str());
        Log::get_instance()->flush();
        int res = mysql_query(mysql, sql_insert.c_str());

        userfd_mp[tar_user] = m_sockfd;

        append_back_data(W_DataPacket(LGN, m_sessionID.length() + 2, (m_sessionID + "\r\n").c_str()));
        m_threadpool->append(this, 1);
    }

    lock.unlock();

    LOG_INFO("user %s login,sessionID is:%s", tar_user.c_str(), m_sessionID.c_str());
    Log::get_instance()->flush();
}

void requestProcess::regis()
{
    int lineCnt = 0;
    int l = m_content_length;

    //如果超出36个字符，说明不合法
    if (l > 36)
    {
        append_back_data(W_DataPacket(RGT, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    //解析用户名和密码
    string tar_user = "";
    string password = "";
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            lineCnt++;
            i++;
            continue;
        }
        switch (lineCnt)
        {
        case 0:
            tar_user += m_content[i];
            break;
        case 1:
            password += m_content[i];
            break;
        default:
            break;
        }
    }

    //判断用户名和密码是否合法
    bool isNice = true;

    if (tar_user.length() > 16 || tar_user == "" || password.length() > 16 || password == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : tar_user)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
        for (const auto &c : password)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        append_back_data(W_DataPacket(RGT, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    lock.lock();

    auto it = user_mp.find(tar_user);

    if (it != user_mp.end()) //用户名被占用
    {
        append_back_data(W_DataPacket(RGT, 3, "0\r\n"));
        m_threadpool->append(this, 1);
    }
    else
    {
        string sql_insert = "INSERT INTO user_tb(user_name, user_password) VALUES('" + tar_user + "', '" + password + "')";
        //从连接池中取一个连接
        MYSQL *mysql;
        connectionRAII mysqlcon(&mysql, m_connPool);

        LOG_INFO("do mysql query:%s", sql_insert.c_str());
        Log::get_instance()->flush();
        int res = mysql_query(mysql, sql_insert.c_str());

        user_mp.insert(pair<string, string>(tar_user, password));

        append_back_data(W_DataPacket(RGT, 3, "1\r\n"));
        m_threadpool->append(this, 1);

        LOG_INFO("register a user :%s", tar_user.c_str());
        Log::get_instance()->flush();
    }

    lock.unlock();
}

void requestProcess::logout()
{
    if (m_sessionID == "")
    {
        return;
    }

    lock.lock();
    auto it = sessionID_mp.find(m_sessionID);

    if (it != sessionID_mp.end())
    {
        LOG_INFO("user %s logout", it->second.c_str());
        Log::get_instance()->flush();

        userfd_mp.erase(sessionID_mp[m_sessionID]);
        sessionID_mp.erase(it);

        string sql_delete = "DELETE FROM sessionID_tb WHERE sessionID = '" + m_sessionID + "'";
        //从连接池中取一个连接
        MYSQL *mysql;
        connectionRAII mysqlcon(&mysql, m_connPool);
        LOG_INFO("do mysql query:%s", sql_delete.c_str());
        Log::get_instance()->flush();
        int res = mysql_query(mysql, sql_delete.c_str());
    }

    lock.unlock();
}

//处理搜索用户逻辑
void requestProcess::search_user()
{
    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(SCU, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    int l = m_content_length;

    //如果超出18个字符，说明不合法
    if (l > 18)
    {
        append_back_data(W_DataPacket(SCU, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    //解析用户名
    string tar_user = "";
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            break;
        }
        tar_user += m_content[i];
    }

    //判断用户名是否合法
    bool isNice = true;

    if (tar_user.length() > 16 || tar_user == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : tar_user)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        append_back_data(W_DataPacket(SCU, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    lock.lock();

    auto it = user_mp.find(tar_user);

    if (it != user_mp.end()) //用户名存在
    {
        append_back_data(W_DataPacket(SCU, 3, "1\r\n"));
        string f_path = "datas/avatar/" + tar_user + ".png";
        // 这是一个存储文件(夹)信息的结构体，其中有文件大小和创建时间、访问时间、修改时间等
        struct stat statbuf;
        // 提供文件名字符串，获得文件属性结构体
        stat(f_path.c_str(), &statbuf);
        int f_size = statbuf.st_size;
        fstream fs(f_path, ios::in | ios::binary);
        char *content = new char[tar_user.length() + 2 + f_size];
        memcpy(content, (tar_user + "\r\n").c_str(), tar_user.length() + 2);
        if (fs.is_open())
        {
            fs.read(content + tar_user.length() + 2, f_size);
            append_back_data(W_DataPacket(RAV, tar_user.length() + 2 + f_size, content));
        }
        fs.close();
        delete[] content;
        m_threadpool->append(this, 1);
    }
    else //用户名不存在
    {
        append_back_data(W_DataPacket(SCU, 3, "0\r\n"));
        m_threadpool->append(this, 1);
    }

    lock.unlock();
}

//处理添加好友逻辑
void requestProcess::add_friend()
{

    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(ADF, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    int l = m_content_length;

    //如果超出18个字符，说明不合法
    if (l > 18)
    {
        append_back_data(W_DataPacket(ADF, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    //解析用户名
    string tar_user = "";
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            break;
        }
        tar_user += m_content[i];
    }

    //判断用户名是否合法
    bool isNice = true;

    if (tar_user.length() > 16 || tar_user == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : tar_user)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        append_back_data(W_DataPacket(ADF, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    string m_username = sessionID_mp[m_sessionID];

    LOG_INFO("user %s apply to add user %s", m_username.c_str(), tar_user.c_str());
    Log::get_instance()->flush();

    if (tar_user == m_username) //不能添加自己为好友
    {
        append_back_data(W_DataPacket(ADF, 4, "-3\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    lock.lock();

    auto it = user_mp.find(tar_user);
    if (it != user_mp.end()) //用户名存在
    {
        if (friend_mp[tar_user].count(m_username) == 0) //两人不是好友关系
        {
            if (frireq_mp[tar_user].count(m_username) == 0) //没有重复发送好友申请
            {
                if (frireq_mp[m_username].count(tar_user) == 0) //目标用户没有向本用户发送申请
                {
                    frireq_mp[tar_user].insert(m_username);
                    append_back_data(W_DataPacket(ADF, 3, "1\r\n"));
                    m_threadpool->append(this, 1);

                    auto it_tar_user = userfd_mp.find(tar_user);

                    if (it_tar_user != userfd_mp.end()) //用户在线
                    {
                        m_users[it_tar_user->second].append_back_data(W_DataPacket(RFR, m_username.length() + 2, (m_username + "\r\n").c_str()));
                        string f_path = "datas/avatar/" + m_username + ".png";
                        // 这是一个存储文件(夹)信息的结构体，其中有文件大小和创建时间、访问时间、修改时间等
                        struct stat statbuf;
                        // 提供文件名字符串，获得文件属性结构体
                        stat(f_path.c_str(), &statbuf);
                        int f_size = statbuf.st_size;
                        fstream fs(f_path, ios::in | ios::binary);
                        char *content = new char[m_username.length() + 2 + f_size];
                        memcpy(content, (m_username + "\r\n").c_str(), m_username.length() + 2);
                        if (fs.is_open())
                        {
                            fs.read(content + m_username.length() + 2, f_size);
                            m_users[it_tar_user->second].append_back_data(W_DataPacket(RAV, m_username.length() + 2 + f_size, content));
                        }
                        fs.close();
                        delete[] content;
                        m_threadpool->append(m_users + it_tar_user->second, 1);
                    }
                    else
                    {
                        string sql_insert = "INSERT INTO data_not_send_tb(user_name, category,content_len, content) VALUES('" + tar_user + "', '" + to_string(RFR) + "', '" + to_string(m_username.length() + 2) + "', '" + (m_username + "\r\n") + "')";
                        //从连接池中取一个连接
                        MYSQL *mysql;
                        connectionRAII mysqlcon(&mysql, m_connPool);
                        LOG_INFO("do mysql query:%s", sql_insert.c_str());
                        Log::get_instance()->flush();
                        int res = mysql_query(mysql, sql_insert.c_str());

                        sql_insert = "INSERT INTO data_not_send_tb(user_name, category,content_len, content) VALUES('" + tar_user + "', '" + to_string(RAV) + "', '" + to_string(m_username.length()) + "', '" + (m_username) + "')";
                        LOG_INFO("do mysql query:%s", sql_insert.c_str());
                        Log::get_instance()->flush();
                        res = mysql_query(mysql, sql_insert.c_str());
                    }
                }
                else //目标用户已经向本用户发送了好友请求
                {
                    append_back_data(W_DataPacket(ADF, 3, "3\r\n"));
                    m_threadpool->append(this, 1);
                }
            }
            else //已经发过好友申请
            {
                append_back_data(W_DataPacket(ADF, 4, "-1\r\n"));
                m_threadpool->append(this, 1);
            }
        }
        else //两人已经是好友
        {
            append_back_data(W_DataPacket(ADF, 3, "2\r\n"));
            m_threadpool->append(this, 1);
        }
    }
    else //用户名不存在
    {
        append_back_data(W_DataPacket(ADF, 3, "0\r\n"));
        m_threadpool->append(this, 1);
    }

    lock.unlock();
}

//处理回复好友申请逻辑
void requestProcess::reply_friend_request()
{
    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(RFR, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    int l = m_content_length;

    string tar_user = "";
    char choose = '\0';

    //如果超出21个字符，说明不合法
    if (l > 21)
    {
        return;
    }

    //解析用户名和选择
    int lineCnt = 0;
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            lineCnt++;
            i++;
            continue;
        }
        switch (lineCnt)
        {
        case 0:
            tar_user += m_content[i];
            break;
        case 1:
            choose = m_content[i];
            break;
        default:
            break;
        }
    }

    //判断用户名和选择是否合法

    bool isNice = true;

    if (tar_user.length() > 16 || tar_user == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : tar_user)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        return;
    }

    string m_username = sessionID_mp[m_sessionID];

    lock.lock();

    if (frireq_mp[m_username].count(tar_user) == 1) //存在好友申请
    {
        if (choose == '1') //接受好友申请
        {
            if (friend_mp[m_username].count(tar_user) == 0) //不是好友
            {
                string sql_insert = "INSERT INTO friend_tb(usera_name, userb_name) VALUES('" + m_username + "', '" + tar_user + "')";
                //从连接池中取一个连接
                MYSQL *mysql;
                connectionRAII mysqlcon(&mysql, m_connPool);

                LOG_INFO("do mysql query:%s", sql_insert.c_str());
                Log::get_instance()->flush();

                int res = mysql_query(mysql, sql_insert.c_str());

                friend_mp[m_username].insert(tar_user);
                friend_mp[tar_user].insert(m_username);

                append_back_data(W_DataPacket(AFI, tar_user.length() + 2, (tar_user + "\r\n").c_str()));
                string s_time = to_string(timestamp());
                append_back_data(W_DataPacket(RMA, tar_user.length() + 2 + s_time.length() + 2 + 47, (tar_user + "\r\n" + s_time + "\r\n" + "我们已经是好友了，快开始聊天吧\r\n").c_str()));
                m_threadpool->append(this, 1);

                if (userfd_mp.find(tar_user) != userfd_mp.end()) //用户在线
                {
                    m_users[userfd_mp[tar_user]].append_back_data(W_DataPacket(AFI, m_username.length() + 2, (m_username + "\r\n").c_str()));
                    m_users[userfd_mp[tar_user]].append_back_data(W_DataPacket(RMA, m_username.length() + 2 + s_time.length() + 2 + 47, (m_username + "\r\n" + s_time + "\r\n" + "我们已经是好友了，快开始聊天吧\r\n").c_str()));
                    m_threadpool->append(m_users + userfd_mp[tar_user], 1);
                }
                else
                {
                    string sql_insert = "INSERT INTO data_not_send_tb(user_name, category,content_len, content) VALUES('" + tar_user + "', '" + to_string(AFI) + "', '" + to_string(m_username.length() + 2) + "', '" + (m_username + "\r\n") + "')";
                    //从连接池中取一个连接
                    MYSQL *mysql;
                    connectionRAII mysqlcon(&mysql, m_connPool);
                    LOG_INFO("do mysql query:%s", sql_insert.c_str());
                    Log::get_instance()->flush();
                    mysql_query(mysql, "set names utf8");
                    int res = mysql_query(mysql, sql_insert.c_str());
                    sql_insert = "INSERT INTO data_not_send_tb(user_name, category,content_len, content) VALUES('" + tar_user + "', '" + to_string(RMA) + "', '" + to_string(m_username.length() + 2 + s_time.length() + 2 + 47) + "', '" + (m_username + "\r\n" + s_time + "\r\n" + "我们已经是好友了，快开始聊天吧\r\n") + "')";
                    LOG_INFO("do mysql query:%s", sql_insert.c_str());
                    Log::get_instance()->flush();
                    mysql_query(mysql, "set names utf8");
                    res = mysql_query(mysql, sql_insert.c_str());
                }
            }
        }
        else //拒绝好友申请
        {
        }

        frireq_mp[m_username].erase(tar_user);
    }

    lock.unlock();

    LOG_INFO("user %s %s user %s friend request.", m_username.c_str(), (choose == '1' ? "accept" : "reject"), tar_user.c_str());
    Log::get_instance()->flush();
}

void requestProcess::reconnect()
{
    int l = m_content_length;

    string sessionID = "";
    char choose = '\0';

    //如果超出20个字符，说明不合法
    if (l > 20)
    {
        append_back_data(W_DataPacket(RCN, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    //解析sessionID
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            i++;
            continue;
        }
        sessionID += m_content[i];
    }

    //判断sessionID是否合法

    bool isNice = true;

    if (sessionID.length() > 18 || sessionID == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : sessionID)
        {
            if (!isdigit(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        append_back_data(W_DataPacket(RCN, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    int preUser = -1;

    lock.lock();

    if (sessionID_mp.find(sessionID) != sessionID_mp.end()) //存在sessionID
    {
        preUser = userfd_mp[sessionID_mp[sessionID]];
        userfd_mp[sessionID_mp[sessionID]] = m_sockfd;
        m_sessionID = sessionID;

        append_back_data(W_DataPacket(RCN, 3, "1\r\n"));
        m_threadpool->append(this, 1);
    }
    else //不存在sessionID
    {
        append_back_data(W_DataPacket(RCN, 3, "0\r\n"));
        m_threadpool->append(this, 1);
    }

    lock.unlock();

    if (preUser != -1)
    {
        m_users[preUser].close_conn();
    }
}

void requestProcess::get_friend_items()
{
    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(GFI, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    string m_username = sessionID_mp[m_sessionID];

    string content = "";
    lock.lock();

    for (const auto &fri : friend_mp[m_username])
    {
        content += fri + "\r\n";
    }

    append_back_data(W_DataPacket(GFI, content.length(), content.c_str()));
    m_threadpool->append(this, 1);

    lock.unlock();

    LOG_INFO("user %s get friend items", m_username.c_str());
    Log::get_instance()->flush();
}

void requestProcess::delete_friend()
{
    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(DEF, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    int l = m_content_length;

    string tar_user = "";

    //如果超出18个字符，说明不合法
    if (l > 18)
    {
        append_back_data(W_DataPacket(RCN, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }
    //解析username
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            i++;
            continue;
        }
        tar_user += m_content[i];
    }

    //判断username是否合法

    bool isNice = true;

    if (tar_user.length() > 16 || tar_user == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : tar_user)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        append_back_data(W_DataPacket(DEF, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    string m_username = sessionID_mp[m_sessionID];

    lock.lock();

    if (friend_mp[m_username].count(tar_user) != 0) //确实有好友关系
    {
        string sql_delete = "DELETE FROM friend_tb WHERE (usera_name = '" + m_username + "' && userb_name = '" + tar_user + "') || (usera_name = '" + tar_user + "' && userb_name = '" + m_username + "')";
        //从连接池中取一个连接
        MYSQL *mysql;
        connectionRAII mysqlcon(&mysql, m_connPool);

        LOG_INFO("do mysql query:%s", sql_delete.c_str());
        Log::get_instance()->flush();
        int res = mysql_query(mysql, sql_delete.c_str());

        friend_mp[m_username].erase(tar_user);
        friend_mp[tar_user].erase(m_username);

        append_back_data(W_DataPacket(DFI, tar_user.length() + 2, (tar_user + "\r\n").c_str()));
        m_threadpool->append(this, 1);

        if (userfd_mp.find(tar_user) != userfd_mp.end()) //用户在线
        {
            m_users[userfd_mp[tar_user]].append_back_data(W_DataPacket(DFI, m_username.length() + 2, (m_username + "\r\n").c_str()));
            m_threadpool->append(m_users + userfd_mp[tar_user], 1);
        }
        else
        {
            string sql_insert = "INSERT INTO data_not_send_tb(user_name, category,content_len, content) VALUES('" + tar_user + "', '" + to_string(DFI) + "', '" + to_string(m_username.length() + 2) + "', '" + (m_username + "\r\n") + "')";
            //从连接池中取一个连接
            MYSQL *mysql;
            connectionRAII mysqlcon(&mysql, m_connPool);
            LOG_INFO("do mysql query:%s", sql_insert.c_str());
            Log::get_instance()->flush();
            mysql_query(mysql, "set names utf8");
            int res = mysql_query(mysql, sql_insert.c_str());
        }
    }

    lock.unlock();

    LOG_INFO("user %s delete friend %s", m_username.c_str(), tar_user.c_str());
    Log::get_instance()->flush();
}

//处理发送消息逻辑
void requestProcess::send_message()
{

    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(SMA, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    int l = m_content_length;

    //解析目标用户名

    int lineCnt = 0;
    string msg_id = "";
    string tar_user = "";
    string content = "";
    for (int i = 0; i < l - 1; i++)
    {
        if (m_content[i] == '\r' && m_content[i + 1] == '\n')
        {
            lineCnt++;
            if (lineCnt >= 2)
            {
                content = string(m_content + i + 2, m_content + l - 2);
                break;
            }
            i++;
            continue;
        }
        if (lineCnt == 0)
        {
            msg_id += m_content[i];
        }
        else if (lineCnt == 1)
        {
            tar_user += m_content[i];
        }
    }

    //判断用户名是否合法
    bool isNice = true;

    if (tar_user.length() > 16 || tar_user == "")
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : tar_user)
        {
            if (!isdigit(c) && !isalpha(c))
            {
                isNice = false;
            }
        }
    }
    if (!isNice)
    {
        append_back_data(W_DataPacket(SMA, 4, "-2\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    string m_username = sessionID_mp[m_sessionID];

    LOG_INFO("user %s send user %s message:%s", m_username.c_str(), tar_user.c_str(), content.c_str());
    Log::get_instance()->flush();

    if (tar_user == m_username) //不能发送给自己
    {
        append_back_data(W_DataPacket(SMA, 4, "-3\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    lock.lock();

    auto it = user_mp.find(tar_user);
    if (it != user_mp.end()) //用户名存在
    {
        if (friend_mp[tar_user].count(m_username) == 1) //两人是好友关系
        {
            string s_time = to_string(timestamp());
            append_back_data(W_DataPacket(SMA, msg_id.length() + 2 + s_time.length() + 2, (msg_id + "\r\n" + s_time + "\r\n").c_str()));
            m_threadpool->append(this, 1);

            auto it_tar_user = userfd_mp.find(tar_user);

            if (it_tar_user != userfd_mp.end()) //用户在线
            {
                m_users[it_tar_user->second].append_back_data(W_DataPacket(RMA, m_username.length() + 2 + s_time.length() + 2 + content.length() + 2, (m_username + "\r\n" + s_time + "\r\n" + content + "\r\n").c_str()));
                m_threadpool->append(m_users + it_tar_user->second, 1);
            }
            else
            {
                int tmplen = content.length();
                for (int i = 0; i < content.length(); ++i)
                {
                    if (content[i] == '\'')
                    {
                        content.insert(i, "\\");
                        ++i;
                    }
                    else if (content[i] == '\\')
                    {
                        content.insert(i, "\\");
                        ++i;
                    }
                }

                string sql_insert = "INSERT INTO data_not_send_tb(user_name, category,content_len, content) VALUES('" + tar_user + "', '" + to_string(RMA) + "', '" + to_string(m_username.length() + 2 + s_time.length() + 2 + tmplen + 2) + "', '" + (m_username + "\r\n" + s_time + "\r\n" + content + "\r\n") + "')";
                //从连接池中取一个连接
                MYSQL *mysql;
                connectionRAII mysqlcon(&mysql, m_connPool);
                LOG_INFO("do mysql query:%s", sql_insert.c_str());
                Log::get_instance()->flush();
                mysql_query(mysql, "set names utf8");
                int res = mysql_query(mysql, sql_insert.c_str());
            }
        }
        else //两人不是好友
        {
            append_back_data(W_DataPacket(SMA, 3, "0\r\n"));
            m_threadpool->append(this, 1);
        }
    }
    else //用户名不存在
    {
        append_back_data(W_DataPacket(SMA, 4, "-1\r\n"));
        m_threadpool->append(this, 1);
    }

    lock.unlock();
}

//处理客户端就绪逻辑
void requestProcess::client_ready()
{

    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(RDY, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    string m_username = sessionID_mp[m_sessionID];

    //从连接池中取一个连接
    MYSQL *mysql;
    connectionRAII mysqlcon(&mysql, m_connPool);

    //在data_not_send_tb表中检索未发送的数据包
    string sql_select = "SELECT category,content_len,content FROM data_not_send_tb WHERE user_name = '" + m_username + "'";

    LOG_INFO("do mysql query:%s", sql_select.c_str());
    Log::get_instance()->flush();
    mysql_query(mysql, "set names utf8");
    if (mysql_query(mysql, sql_select.c_str()))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    auto result = mysql_store_result(mysql);

    //返回结果集中的列数
    auto num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    auto fields = mysql_fetch_fields(result);

    lock.lock();
    //从结果集中获取下一行
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        string temp3(row[2]);
        if (PACKET_TYPE(stoi(temp1)) == RAV)
        {
            string f_path = "datas/avatar/" + temp3 + ".png";
            // 这是一个存储文件(夹)信息的结构体，其中有文件大小和创建时间、访问时间、修改时间等
            struct stat statbuf;
            // 提供文件名字符串，获得文件属性结构体
            stat(f_path.c_str(), &statbuf);
            int f_size = statbuf.st_size;
            fstream fs(f_path, ios::in | ios::binary);
            char *content = new char[temp3.length() + 2 + f_size];
            memcpy(content, (temp3 + "\r\n").c_str(), temp3.length() + 2);
            if (fs.is_open())
            {
                fs.read(content + temp3.length() + 2, f_size);
                append_back_data(W_DataPacket(PACKET_TYPE(stoi(temp1)), temp3.length() + 2 + f_size, content));
            }
            fs.close();
            delete[] content;
        }
        else
        {
            append_back_data(W_DataPacket(PACKET_TYPE(stoi(temp1)), stoi(temp2), temp3.c_str()));
        }
    }

    m_threadpool->append(this, 1);

    lock.unlock();

    string sql_delete = "DELETE FROM data_not_send_tb WHERE user_name = '" + m_username + "'";

    LOG_INFO("do mysql query:%s", sql_delete.c_str());
    Log::get_instance()->flush();
    if (mysql_query(mysql, sql_delete.c_str()))
    {
        LOG_ERROR("DELETE error:%s\n", mysql_error(mysql));
    }
}

//处理更换头像逻辑
void requestProcess::change_avatar()
{

    if (m_sessionID == "") //没有权限
    {
        append_back_data(W_DataPacket(SAV, 4, "-4\r\n"));
        m_threadpool->append(this, 1);
        return;
    }

    string m_username = sessionID_mp[m_sessionID];

    ofstream outFile;
    outFile.open("datas/avatar/" + m_username + ".png");

    for (int i = 0; i < m_content_length; ++i)
    {
        outFile << m_content[i];
    }
    outFile.close();

    append_back_data(W_DataPacket(SAV, 4, "1\r\n"));
    m_threadpool->append(this, 1);

    int send_content_len = m_username.length() + 2 + m_content_length;
    char *send_content = new char[send_content_len];
    memcpy(send_content, (m_username + "\r\n").c_str(), m_username.length() + 2);
    memcpy(send_content + m_username.length() + 2, m_content, m_content_length);

    W_DataPacket pack(RAV, send_content_len, send_content);

    for (const auto &tar_user : friend_mp[m_username])
    {
        auto it_tar_user = userfd_mp.find(tar_user);

        if (it_tar_user != userfd_mp.end()) //用户在线
        {
            m_users[it_tar_user->second].append_back_data(pack);
            m_threadpool->append(m_users + it_tar_user->second, 1);
        }
        else
        {
            string sql_insert = "INSERT INTO data_not_send_tb(user_name, category,content_len, content) VALUES('" + tar_user + "', '" + to_string(RAV) + "', '" + to_string(m_username.length()) + "', '" + (m_username) + "')";
            //从连接池中取一个连接
            MYSQL *mysql;
            connectionRAII mysqlcon(&mysql, m_connPool);
            LOG_INFO("do mysql query:%s", sql_insert.c_str());
            Log::get_instance()->flush();
            mysql_query(mysql, "set names utf8");
            int res = mysql_query(mysql, sql_insert.c_str());
        }
    }
}

const char *requestProcess::ReqToString(requestProcess::PACKET_TYPE r)
{
    switch (r)
    {
    case HBT:
        return "HBT";
    case LGN:
        return "LGN";
    case RGT:
        return "RGT";
    case LGT:
        return "LGT";
    case SCU:
        return "SCU";
    case ADF:
        return "ADF";
    case DEF:
        return "DEF";
    case RFR:
        return "RFR";
    case RCN:
        return "RCN";
    case GFI:
        return "GFI";
    case AFI:
        return "AFI";
    case DFI:
        return "DFI";
    case SMA:
        return "SMA";
    case RMA:
        return "RMA";
    case RDY:
        return "RDY";
    case SAV:
        return "SAV";
    case RAV:
        return "RAV";
    default:
        return "ERR";
    }
}

long long requestProcess::timestamp()
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000 + tv.tv_usec / 1000);
}
