#ifndef SQL_CONNECTION_POOL_H
#define SQL_CONNECTION_POOL_H

#include <mysql/mysql.h>
#include <string>
#include <list>

#include "../locker/locker.h"
class connectionPool
{
public:
    ~connectionPool();

    //获取数据库连接
    MYSQL *get_connection();

    //释放连接
    bool release_connection(MYSQL *connection);

    //获取连接
    int get_free_connection();

    //销毁数据库连接池
    void destroy_pool();

    //单例懒汉模式
    static connectionPool *get_instance();

    void init(std::string url, std::string User, std::string PassWord, std::string DataBaseName, int Port, unsigned int MaxConn);

private:
    connectionPool();

private:
    //最大连接数
    int m_max_conn;
    //当前已使用的连接数
    int m_cur_conn;
    //当前空闲的连接数
    int m_free_conn;
    //主机地址
    std::string m_url;
    //数据库端口号
    std::string m_port;
    //登录数据库用户名
    std::string m_user;
    //登录数据库密码
    std::string m_password;
    //使用数据库名
    std::string m_database_name;
    //互斥锁
    mutexLocker m_lock;
    //连接池
    std::list<MYSQL *> m_connList;
    //信号量
    sem m_reserve;
};

// RAII类
class connectionRAII
{
public:
    connectionRAII(MYSQL **con, connectionPool *connPool);

    ~connectionRAII();

private:
    MYSQL *m_conn_RAII;
    connectionPool *m_pool_RAII;
};

#endif // SQL_CONNECTION_POOL_H