#include "db.h"
#include "config.hpp"
#include <muduo/base/Logging.h>

// 初始化数据库连接
MySQL::MySQL()
{
    _conn = mysql_init(nullptr);
}

// 释放数据库连接资源
MySQL::~MySQL()
{
    if (_conn != nullptr)
        mysql_close(_conn);
}

// 连接数据库
bool MySQL::connect()
{
    const string server = chatserver::config::envOr("CHAT_MYSQL_HOST", "127.0.0.1");
    const string user = chatserver::config::envOr("CHAT_MYSQL_USER", "root");
    const string password = chatserver::config::envOr("CHAT_MYSQL_PASSWORD", "123456");
    const string dbname = chatserver::config::envOr("CHAT_MYSQL_DATABASE", "chat");
    const int port = chatserver::config::envIntOr("CHAT_MYSQL_PORT", 3306);

    MYSQL *p = mysql_real_connect(_conn, server.c_str(), user.c_str(),
                                  password.c_str(), dbname.c_str(), port, nullptr, 0);
    if (p != nullptr)
    {
        mysql_set_character_set(_conn, "utf8mb4");
    }
    else
    {
        LOG_ERROR << "connect mysql failed: " << mysql_error(_conn)
                  << " host=" << server << ":" << port
                  << " database=" << dbname;
    }

    return p;
}

// 更新操作
bool MySQL::update(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        // 不打印完整 SQL，避免把密码哈希或聊天内容写入日志。
        LOG_ERROR << "mysql update failed: " << mysql_error(_conn);
        return false;
    }

    return true;
}

// 查询操作
MYSQL_RES *MySQL::query(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_ERROR << "mysql query failed: " << mysql_error(_conn);
        return nullptr;
    }
    
    return mysql_use_result(_conn);
}

// 获取连接
MYSQL* MySQL::getConnection()
{
    return _conn;
}
