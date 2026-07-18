#include "offlinemessagemodel.hpp"
#include "db.h"
#include <muduo/base/Logging.h>
#include <cstring>
using namespace muduo;

// 离线消息模型：
// - 安全转义消息内容
// - 落库/删除失败记录日志
// - insert 返回 bool 供上层判断

// ============================================================
// 存储用户的离线消息
// 对 msg 做 mysql_real_escape_string 转义，防止单引号等特殊字符
// 导致 SQL 语句错误（等效于参数绑定）
// 返回 true 表示落库成功，false 表示失败（调用方应记录告警/触发重试）
// ============================================================
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

// ============================================================
// 删除用户的离线消息
// 删除失败记录错误日志
// ============================================================
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

// ============================================================
// 查询用户的离线消息
// ============================================================
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
