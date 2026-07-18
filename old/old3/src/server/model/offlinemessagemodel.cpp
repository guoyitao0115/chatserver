#include "offlinemessagemodel.hpp"
#include "db.h"
#include <muduo/base/Logging.h>
#include <cstring>
using namespace muduo;

// ============================================================
// 【改动6】离线消息模型 — 增强落库可靠性
// ============================================================
// 原问题：
//   1. sprintf 拼接 SQL，消息体中若含单引号 (') 会导致 SQL 注入或写入失败
//   2. insert 失败时静默忽略，调用方无法感知丢消息
//
// 修改内容：
//   1. insert 使用 mysql_real_escape_string 对消息内容转义，
//      等效于参数化查询（hiredis/MySQL C API 不支持 prepared stmt 的简便包装）
//   2. insert/remove 失败时记录 LOG_ERROR，便于排查
//   3. insert 返回 bool，调用方可判断是否成功
// ============================================================

// ============================================================
// 存储用户的离线消息
// 【改动6】对 msg 做 mysql_real_escape_string 转义，防止单引号等特殊字符
//          导致 SQL 语句错误（等效于参数绑定）
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

    // 【改动6】对消息内容做转义，防止消息中含有单引号/反斜杠破坏SQL结构
    // mysql_real_escape_string 需要已建立的连接句柄
    size_t msgLen = msg.size();
    // 转义后最长为原来2倍+1
    vector<char> escaped(msgLen * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(),
                             escaped.data(),
                             msg.c_str(),
                             (unsigned long)msgLen);

    // 组装安全的SQL语句
    char sql[4096] = {0};
    snprintf(sql, sizeof(sql),
             "INSERT INTO offlinemessage VALUES(%d, '%s')",
             userid, escaped.data());

    if (!mysql.update(sql))
    {
        // 【改动6】落库失败必须记录错误日志，不能静默丢弃
        LOG_ERROR << "[OfflineMsgModel::insert] insert failed, userid="
                  << userid << " sql=" << sql;
        return false;
    }
    return true;
}

// ============================================================
// 删除用户的离线消息
// 【改动6】删除失败记录错误日志
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
             "SELECT message FROM offlinemessage WHERE userid = %d", userid);

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
