#include "db.h"
#include "config.hpp"
#include <muduo/base/Logging.h>

/**
 * @brief 创建一个 MySQL C API 连接句柄。
 *
 * mysql_init 只初始化本地句柄，真正的 TCP 连接由 connect() 建立。
 * 每个 Model 方法都在栈上创建 MySQL 对象，因此句柄不在多线程之间共享。
 */
MySQL::MySQL()
{
    _conn = mysql_init(nullptr);
}

/**
 * @brief 释放连接句柄，保证所有早退路径都不泄漏数据库资源。
 *
 * Model 层依赖 RAII：无论 SQL 成功还是失败，函数返回时析构函数都会
 * 执行 mysql_close。
 */
MySQL::~MySQL()
{
    if (_conn != nullptr)
        mysql_close(_conn);
}

/**
 * @brief 根据环境变量建立数据库连接。
 * @return 连接成功返回 true，否则记录目标主机和 MySQL 错误并返回 false。
 *
 * 配置优先从 CHAT_MYSQL_* 环境变量读取，未设置时使用本地开发默认值。
 * 连接成功后设置 utf8mb4，以完整保存中文和 emoji，避免聊天内容乱码。
 */
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

/**
 * @brief 执行不需要返回结果集的 SQL，如 INSERT/UPDATE/DELETE。
 * @param sql 已由上层按字段类型组装并对字符串进行转义的 SQL。
 * @return mysql_query 执行成功返回 true。
 *
 * 错误日志故意不输出完整 SQL，因为 SQL 可能包含密码哈希或聊天正文。
 */
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

/**
 * @brief 执行 SELECT 并返回流式结果集。
 * @return 成功时返回 mysql_use_result 的 MYSQL_RES*，失败返回 nullptr。
 *
 * mysql_use_result 不会一次性把整个结果集搬入内存，但调用方必须完整读取并
 * mysql_free_result，且在释放前不能在同一连接上发送新查询。
 */
MYSQL_RES *MySQL::query(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_ERROR << "mysql query failed: " << mysql_error(_conn);
        return nullptr;
    }
    
    return mysql_use_result(_conn);
}

/**
 * @brief 暴露当前 MYSQL 句柄，供字符串转义和获取自增主键使用。
 *
 * 返回值的生命周期不超过当前 MySQL 对象，调用方不应缓存该指针。
 */
MYSQL* MySQL::getConnection()
{
    return _conn;
}
