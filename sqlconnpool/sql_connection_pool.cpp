#include "sql_connection_pool.h"
#include "../log/log.h"

using namespace std;

connectionPool::connectionPool()
{
    this->m_cur_conn = 0;
    this->m_free_conn = 0;
}

connectionPool::~connectionPool()
{
    destroy_pool();
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connectionPool::connectionPool::get_connection()
{
    MYSQL *connection = NULL;

    if (0 == m_connList.size())
    {
        return NULL;
    }

    m_reserve.wait();

    m_lock.lock();

    connection = m_connList.front();
    m_connList.pop_front();

    --m_free_conn;
    ++m_cur_conn;

    m_lock.unlock();

    if (mysql_query(connection, "delete from empty_table") != 0) //如果连接不可用，则重连
    {
        connection = mysql_init(connection);

        if (connection == NULL)
        {
            printf("Error: %s\n", mysql_error(connection));
            exit(1);
        }
        connection = mysql_real_connect(connection, m_url.c_str(), m_user.c_str(), m_password.c_str(),
                                        m_database_name.c_str(), m_port, NULL, 0);

        if (connection == NULL)
        {
            printf("Error: %s\n", mysql_error(connection));
            exit(1);
        }

        LOG_INFO("reconnect a sqlconnect");
        Log::get_instance()->flush();
    }
    return connection;
}

bool connectionPool::release_connection(MYSQL *connection)
{
    if (NULL == connection)
    {
        return false;
    }

    m_lock.lock();

    m_connList.push_back(connection);
    ++m_free_conn;
    --m_cur_conn;

    m_lock.unlock();

    m_reserve.post();

    return true;
}

int connectionPool::get_free_connection()
{
    return this->m_free_conn;
}

void connectionPool::destroy_pool()
{
    m_lock.lock();
    if (!m_connList.empty())
    {
        list<MYSQL *>::iterator it;
        for (it = m_connList.begin(); it != m_connList.end(); ++it)
        {
            MYSQL *con = *it;
            mysql_close(con);
        }
        m_cur_conn = 0;
        m_free_conn = 0;
        m_connList.clear();
    }

    m_lock.unlock();
}

connectionPool *connectionPool::connectionPool::get_instance()
{
    static connectionPool connPool;
    return &connPool;
}

void connectionPool::init(std::string url, std::string User, std::string PassWord, std::string DataBaseName, int Port, unsigned int MaxConn)
{
    this->m_url = url;
    this->m_user = User;
    this->m_password = PassWord;
    this->m_database_name = DataBaseName;
    this->m_port = Port;

    //操作连接池，加锁
    m_lock.lock();
    for (int i = 0; i < MaxConn; ++i)
    {
        MYSQL *con = NULL;
        con = mysql_init(con);

        if (con == NULL)
        {
            printf("Error: %s\n", mysql_error(con));
            exit(1);
        }
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(),
                                 DataBaseName.c_str(), Port, NULL, 0);

        if (con == NULL)
        {
            printf("Error: %s\n", mysql_error(con));
            exit(1);
        }

        m_connList.push_back(con);
        ++m_free_conn;
    }

    m_reserve = sem(m_free_conn);

    this->m_max_conn = m_free_conn;

    m_lock.unlock();
}

connectionRAII::connectionRAII(MYSQL **con, connectionPool *connPool)
{
    *con = connPool->get_connection();

    m_conn_RAII = *con;
    m_pool_RAII = connPool;
}

connectionRAII::~connectionRAII()
{
    m_pool_RAII->release_connection(m_conn_RAII);
}
