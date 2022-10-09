#include "request_process.h"

#include <map>

using namespace std;

int requestProcess::m_user_count = 0;
int requestProcess::m_epollfd = -1;

//将表中的用户名和密码放入map
map<string, string> user_mp;

// sessionID的map
map<long long, string> sessionID_mp;

// 储存用户名对应的描述符
map<string, string> userfd_mp;

mutexLocker m_lock;       //互斥锁
readWriteLocker m_rwlock; //读写锁

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

    init();
}

void requestProcess::init()
{
    m_bytes_have_send = 0;
    m_bytes_to_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;

    m_method = HBT;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    m_content_length = 0;
    m_sessionID = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
}

void requestProcess::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; //关闭一个连接时，将客户总量减1
    }
}

void requestProcess::process()
{
    RESULT_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_connfd_Trig_mode);
        return;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_connfd_Trig_mode);
}

bool requestProcess::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    if (m_connfd_Trig_mode)
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                              READ_BUFFER_SIZE - m_read_idx, 0);

            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                else
                {
                    return false;
                }
            }
            else if (bytes_read == 0)
            {
                return false;
            }

            m_read_idx += bytes_read;
        }
        return true;
    }
    else
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);

        m_read_idx += bytes_read;
        if (bytes_read <= 0)
        {
            return false;
        }
        else
        {
            return true;
        }
    }
}

bool requestProcess::write()
{
    int temp = 0;
    if (m_bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_connfd_Trig_mode);
        init();
        return true;
    }

    while (1)
    {
        temp = send(m_sockfd, m_write_buf, m_write_idx, 0);
        if (temp < 0)
        {
            /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，
            虽然在此期间，服务器无法立即接收到同一客户的下一个请求，
            但这可以保证链接的完整性*/
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_connfd_Trig_mode);
                return true;
            }
            else
            {
                unmap();
                return false;
            }
        }
        m_bytes_to_send -= temp;
        m_bytes_have_send += temp;

        if (m_bytes_to_send <= 0)
        {

            //发送响应成功
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_connfd_Trig_mode);
            //长连接
            init();
            return true;
        }
    }
}

sockaddr_in *requestProcess::get_address()
{
    return &m_address;
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
}

requestProcess::RESULT_CODE requestProcess::process_read()
{
    LINE_STATUS linestatus = LINE_OK; //记录当前行的读取状态
    RESULT_CODE retcode = NO_REQUEST; //记录请求的处理结果
    char *text = 0;

    //主状态机，用于从buffer中取出所有完整的行
    while (((m_check_state == CHECK_STATE_CONTENT) && (linestatus == LINE_OK)) ||
           ((linestatus = parse_line()) == LINE_OK))
    {
        text = get_line();            // start_line是行在buffer中 的起始位置
        m_start_line = m_checked_idx; //记录下一行的起始位置

        LOG_INFO("%s", text);
        Log::get_instance()->flush();

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE: //第一个状态，分析请求行
            retcode = parse_request_line(text);
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
        case CHECK_STATE_CONTENT:
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

bool requestProcess::process_write(RESULT_CODE ret)
{
    //("process_write %d\n", ret);
    switch (ret)
    {
    case GET_REQUEST:
        add_status_line(m_response_status);
        add_headers(m_response_content_len);
        if (!add_content(m_response_content))
        {
            return false;
        }
        break;
    case INTERNAL_ERROR:
        add_status_line("SWR");
        add_headers(0);
        if (!add_content(""))
        {
            return false;
        }
        break;
    case BAD_REQUEST:
        add_status_line("BRQ");
        add_headers(0);
        if (!add_content(""))
        {
            return false;
        }
        break;

    default:
        return false;
    }
    m_bytes_to_send = m_write_idx;
    return true;
}

requestProcess::RESULT_CODE requestProcess::parse_request_line(char *text)
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
    //遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        /*如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
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
    //处理"sessionID"头部字段
    else if (strncasecmp(text, "sessionID:", 10) == 0)
    {
        text += 10;
        text += strspn(text, " \t");
        m_sessionID = atoll(text);
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
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    else
    {
        return NO_REQUEST;
    }
}

requestProcess::RESULT_CODE requestProcess::do_request()
{
    switch (m_method)
    {
    case HBT:
        strcpy(m_response_status, "HBT");
        m_response_content_len = m_content_length;
        strncpy(m_response_content, m_string, m_content_length);
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
    default:
        break;
    }
    return GET_REQUEST;
}

char *requestProcess::get_line()
{
    return m_read_buf + m_start_line;
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
            //否则，就说明客户发送的HTTP请求存在语法问题
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
            //否则，就说明客户发送的HTTP请求存在语法问题
            else
            {
                return LINE_BAD;
            }
        }
    }
    return LINE_OPEN;
}

void requestProcess::unmap()
{
}

bool requestProcess::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx,
                        WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

bool requestProcess::add_content(const char *content)
{
    return add_response("%s", content);
}

bool requestProcess::add_status_line(const char *status)
{
    return add_response("%s\r\n", status);
}

bool requestProcess::add_headers(int content_length)
{
    add_content_length(content_length);
    add_blank_line();
}

bool requestProcess::add_content_length(int content_length)
{
    return add_response("Content-Length: %d\r\n", content_length);
}

bool requestProcess::add_blank_line()
{
    return add_response("%s", "\r\n");
}

void requestProcess::login()
{
    int l = strlen(m_string);

    string username = "";
    string password = "";

    //如果超出36个字符，说明不合法
    if (l > 36)
    {
        strcpy(m_response_status, "LGN");
        m_response_content_len = 4;
        strcpy(m_response_content, "-2\r\n");
        return;
    }

    //解析用户名和密码
    int lineCnt = 0;
    for (int i = 0; i < l - 1; i++)
    {
        if (m_string[i] == '\r' && m_string[i + 1] == '\n')
        {
            lineCnt++;
            i++;
            continue;
        }
        switch (lineCnt)
        {
        case 0:
            username += m_string[i];
            break;
        case 1:
            password += m_string[i];
            break;
        default:
            break;
        }
    }

    //判断用户名和密码是否合法

    bool isNice = true;

    if (username.length() > 16 || password.length() > 16)
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : username)
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
        strcpy(m_response_status, "LGN");
        m_response_content_len = 4;
        strcpy(m_response_content, "-2\r\n");
        return;
    }

    m_lock.lock();

    auto it = user_mp.find(username);

    if (it == user_mp.end() || it->second != password) //用户名或密码错误
    {
        m_sessionID = 0;

        strcpy(m_response_status, "LGN");
        m_response_content_len = 3;
        strcpy(m_response_content, "0\r\n");
    }
    else if (userfd_mp.find(username) != userfd_mp.end()) //用户在线
    {
        m_sessionID = -1;

        strcpy(m_response_status, "LGN");

        m_response_content_len = 4;
        strcpy(m_response_content, "-1\r\n");
    }
    else
    {
        long long cur_time = time(0);
        m_sessionID = cur_time * 1000000 + cur_time % rand() % 1000000;

        sessionID_mp[m_sessionID] = username;
        userfd_mp[username] = m_sockfd;

        strcpy(m_response_status, "LGN");
        m_response_content_len = to_string(m_sessionID).length() + 2;
        strcpy(m_response_content, (to_string(m_sessionID) + "\r\n").c_str());
    }

    m_lock.unlock();

    LOG_INFO("sessionID:%lld", m_sessionID);
    Log::get_instance()->flush();
}

void requestProcess::regis()
{
    int lineCnt = 0;
    int l = strlen(m_string);

    //如果超出36个字符，说明不合法
    if (l > 36)
    {
        strcpy(m_response_status, "RGT");
        m_response_content_len = 4;
        strcpy(m_response_content, "-2\r\n");
        return;
    }

    //解析用户名和密码
    string username = "";
    string password = "";
    for (int i = 0; i < l - 1; i++)
    {
        if (m_string[i] == '\r' && m_string[i + 1] == '\n')
        {
            lineCnt++;
            i++;
            continue;
        }
        switch (lineCnt)
        {
        case 0:
            username += m_string[i];
            break;
        case 1:
            password += m_string[i];
            break;
        default:
            break;
        }
    }

    //判断用户名和密码是否合法
    bool isNice = true;

    if (username.length() > 16 || password.length() > 16)
    {
        isNice = false;
    }
    else
    {
        for (const auto &c : username)
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
        strcpy(m_response_status, "RGT");
        m_response_content_len = 4;
        strcpy(m_response_content, "-2\r\n");
        return;
    }

    m_lock.lock();

    auto it = user_mp.find(username);

    if (it != user_mp.end()) //用户名被占用
    {
        strcpy(m_response_status, "RGT");
        m_response_content_len = 3;
        strcpy(m_response_content, "0\r\n");
    }
    else
    {
        string sql_insert = "INSERT INTO user_tb(user_name, user_password) VALUES('" + username + "', '" + password + "')";

        //从连接池中取一个连接
        MYSQL *mysql;
        connectionRAII mysqlcon(&mysql, m_connPool);
        int res = mysql_query(mysql, sql_insert.c_str());
        user_mp.insert(pair<string, string>(username, password));

        strcpy(m_response_status, "RGT");
        m_response_content_len = 3;
        strcpy(m_response_content, "1\r\n");

        LOG_INFO("register:%s", username.c_str());
        Log::get_instance()->flush();
    }

    m_lock.unlock();
}

void requestProcess::logout()
{
    if (m_sessionID <= 0)
    {
        strcpy(m_response_status, "LGT");
        m_response_content_len = 3;
        strcpy(m_response_content, "0\r\n");
        return;
    }

    m_lock.lock();
    auto it = sessionID_mp.find(m_sessionID);

    if (it == sessionID_mp.end())
    {
        strcpy(m_response_status, "LGT");
        m_response_content_len = 3;
        strcpy(m_response_content, "0\r\n");
    }
    else
    {
        sessionID_mp.erase(it);
        strcpy(m_response_status, "LGT");
        m_response_content_len = 3;
        strcpy(m_response_content, "1\r\n");
    }

    m_lock.unlock();
}