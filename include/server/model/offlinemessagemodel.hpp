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

// 提供离线消息表的操作接口方法
class OfflineMsgModel
{
public:
    // 【改动6】存储用户的离线消息，返回是否落库成功
    bool insert(int userid, string msg);

    // 删除用户的离线消息
    void remove(int userid);

    // 查询用户的离线消息
    vector<string> query(int userid);
};

#endif
