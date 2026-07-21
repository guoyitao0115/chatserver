#ifndef DB_H
#define DB_H

#include <mysql/mysql.h>
#include <string>
using namespace std;

/**
 * MySQL C API 的轻量 RAII 包装。
 *
 * 构造函数只创建本地 MYSQL 句柄，connect() 才建立网络连接；析构函数统一关闭连接。
 * 当前 Model 层通常为一次业务方法创建一个 MySQL 对象，因此连接和流式结果集不会
 * 跨线程共享。该类没有显式拷贝控制，也没有内部锁，禁止复制及并发使用同一实例。
 */
class MySQL
{
public:
    // 调用 mysql_init 创建句柄；此时尚未读取配置或连接服务器。
    MySQL();
    // 关闭 _conn；调用方必须先释放由 query() 返回的全部结果集。
    ~MySQL();

    /**
     * 从 CHAT_MYSQL_* 环境变量读取地址、端口、账号和库名并建立连接。
     * 成功后把连接字符集切换为 utf8mb4，以保存中文和 emoji；失败记录脱敏日志并
     * 返回 false。后续 update/query/getConnection 只应在成功连接后使用。
     */
    bool connect();

    /**
     * 执行 INSERT/UPDATE/DELETE 等不需要结果集的 SQL。
     * @return mysql_query 返回成功时为 true；失败时只记录 MySQL 错误，不输出完整
     * SQL，避免密码哈希和聊天内容进入日志。
     * @note 本函数不负责参数绑定或转义，字符串字段必须由调用方先使用当前连接的
     * mysql_real_escape_string 处理。
     */
    bool update(string sql);

    /**
     * 执行 SELECT 并通过 mysql_use_result 返回流式结果集。
     * @return 成功时返回 MYSQL_RES*，失败为 nullptr。
     * @ownership 返回指针由调用方拥有，必须完整读取并调用 mysql_free_result；释放
     * 之前不能在同一连接上执行下一条语句，且结果集不能比 MySQL 对象存活更久。
     */
    MYSQL_RES *query(string sql);

    /**
     * 借用底层句柄，供 Model 执行字符串转义或读取自增主键。
     * 返回指针不转移所有权、不得缓存或关闭，只在当前 MySQL 对象生命周期内有效。
     */
    MYSQL* getConnection();
private:
    MYSQL *_conn; // 本对象独占的 C API 连接句柄，由析构函数 mysql_close
};

#endif
