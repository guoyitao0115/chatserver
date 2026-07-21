#ifndef OFFLINEMESSAGEMODEL_H
#define OFFLINEMESSAGEMODEL_H

/*
 * ============================================================
 * 【改动6】离线消息模型头文件
 * ============================================================
 * insert() 返回值由 void 改为 bool：
 *   true  —— 落库成功
 *   false —— 落库失败（调用方可据此触发告警或重试）
 * ============================================================
 */

#include <string>
#include <vector>
using namespace std;

// OfflineMsgModel 封装 offlinemessage 表。
// 可靠性重点在于：接收者离线时必须成功落库，登录拉取后再删除，避免消息丢失或重复重放。
class OfflineMsgModel
{
public:
    // 存储用户的离线消息，返回是否落库成功。失败时业务层应返回 ACK_FAIL，允许客户端重试。
    bool insert(int userid, string msg);

    // 删除用户已消费的离线消息。当前粒度是按用户清空，调用时机在登录响应成功组装之后。
    void remove(int userid);

    // 查询用户的离线消息，按数据库自增 id 顺序返回，保证离线消息恢复时保持发送顺序。
    vector<string> query(int userid);
};

#endif
