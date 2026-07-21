#include "offlinemessagemodel.hpp"
#include "db.h"
#include <muduo/base/Logging.h>
#include <cstring>
using namespace muduo;

// 离线消息模型是投递链路的最后一道可靠性兜底：本地连接不存在、
// 跨节点路由缺失或 RabbitMQ 发布失败时，ChatService 会把原始 JSON 写入此表。

/**
 * @brief 按用户 ID 持久化一条完整聊天消息。
 * @param userid 最终接收者 ID。
 * @param msg 待恢复投递的 JSON 字符串，上线后会原样返回客户端。
 * @return 只有数据库连接和 INSERT 都成功时返回 true。
 *
 * 转义缓冲区按 2*n+1 分配，避免固定长度数组截断长消息。
 * mysql_real_escape_string 使用已连接句柄，会按当前字符集正确处理特殊字符。
 * 失败日志会带 userid，但不记录消息正文，以减少隐私泄漏。
 */
bool OfflineMsgModel::insert(int userid, string msg)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        LOG_ERROR << "[OfflineMsgModel::insert] connect DB failed, userid="
                  << userid << " msg dropped!";
        return false;
    }

    // 对消息内容做转义，防止消息中含有单引号/反斜杠破坏 SQL 结构
    // mysql_real_escape_string 需要已建立的连接句柄
    size_t msgLen = msg.size();
    // 转义后最长为原来2倍+1
    vector<char> escaped(msgLen * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(),
                             escaped.data(),
                             msg.c_str(),
                             (unsigned long)msgLen);

    // 动态组装，避免固定 4KB 缓冲区截断较长消息。
    string sql = "INSERT INTO offlinemessage(userid, message) VALUES(";
    sql += to_string(userid);
    sql += ", '";
    sql += escaped.data();
    sql += "')";

    if (!mysql.update(sql))
    {
        // 落库失败必须记录错误日志，不能静默丢弃
        LOG_ERROR << "[OfflineMsgModel::insert] insert failed, userid=" << userid;
        return false;
    }
    return true;
}

/**
 * @brief 删除用户当前全部离线消息。
 *
 * 登录响应已组装完离线消息后调用此函数。当前是“先读取、再删除、
 * 再发送”的简化语义：删除失败可能导致下次重复投递，因此客户端仍需去重；
 * 而删除后、响应到达前进程崩溃，则仍存在丢失窗口。
 */
void OfflineMsgModel::remove(int userid)
{
    char sql[256] = {0};
    snprintf(sql, sizeof(sql),
             "DELETE FROM offlinemessage WHERE userid=%d", userid);

    MySQL mysql;
    if (mysql.connect())
    {
        if (!mysql.update(sql))
        {
            // 删除失败只记录警告，不影响业务流程
            LOG_ERROR << "[OfflineMsgModel::remove] delete failed, userid=" << userid;
        }
    }
    else
    {
        LOG_ERROR << "[OfflineMsgModel::remove] connect DB failed, userid=" << userid;
    }
}

/**
 * @brief 按入库顺序取出用户的所有离线消息。
 * @return 消息 JSON 字符串列表；查询失败时返回空列表。
 *
 * ORDER BY id ASC 使结果按自增主键顺序恢复，保留单库写入的先后关系。
 * 每行的 message 被复制到 vector 后即可释放 MYSQL_RES。
 */
vector<string> OfflineMsgModel::query(int userid)
{
    char sql[256] = {0};
    snprintf(sql, sizeof(sql),
             "SELECT message FROM offlinemessage WHERE userid = %d ORDER BY id ASC", userid);

    vector<string> vec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            // 把userid用户的所有离线消息放入vec中返回
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                vec.push_back(row[0]);
            }
            mysql_free_result(res);
        }
    }
    else
    {
        LOG_ERROR << "[OfflineMsgModel::query] connect DB failed, userid=" << userid;
    }
    return vec;
}
