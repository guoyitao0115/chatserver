#ifndef CHATSERVICE_H
#define CHATSERVICE_H

/*
 * ============================================================
 * 【改动综合】ChatService 头文件
 * 新增成员：
 *   1. _dedup         ——【改动4】消息去重器（LRU+TTL）
 *   2. handleMsgAck() ——【改动2/3】处理客户端 ACK 回执（预留扩展点）
 *   3. sendAck()      ——【改动2】向客户端发送服务端接收确认（MSG_ACK）
 *   4. deliverMsg()   ——【改动5】统一消息投递（含Redis兜底策略）
 * ============================================================
 */

#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include <mutex>
using namespace std;
using namespace muduo;
using namespace muduo::net;

#include "redis.hpp"
#include "groupmodel.hpp"
#include "friendmodel.hpp"
#include "usermodel.hpp"
#include "offlinemessagemodel.hpp"
#include "msgdedup.hpp"   // 【改动4】消息去重模块
#include "json.hpp"
using json = nlohmann::json;

// 表示处理消息的事件回调方法类型
using MsgHandler = std::function<void(const TcpConnectionPtr &conn, json &js, Timestamp)>;

// 聊天服务器业务类
class ChatService
{
public:
    // 获取单例对象的接口函数
    static ChatService *instance();

    // 处理登录业务
    void login(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理注册业务
    void reg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 一对一聊天业务
    void oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 添加好友业务
    void addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 创建群组业务
    void createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 加入群组业务
    void addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 群组聊天业务
    void groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理注销业务
    void loginout(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理客户端异常退出
    void clientCloseException(const TcpConnectionPtr &conn);
    // 服务器异常，业务重置方法
    void reset();
    // 获取消息对应的处理器
    MsgHandler getHandler(int msgid);
    // 从redis消息队列中获取订阅的消息
    void handleRedisSubscribeMessage(int userid, string msg);

private:
    ChatService();

    /*
     * 【改动2/3】向客户端发送服务端接收确认（MSG_ACK）
     * 参数：
     *   conn      —— 目标连接
     *   msgId     —— 原消息的 message_id
     *   ackState  —— ACK状态码（参见 public.hpp AckState 枚举）
     */
    void sendAck(const TcpConnectionPtr &conn, const string &msgId, int ackState);

    /*
     * 【改动5】统一消息投递函数
     * 优先本节点直接推送，否则通过Redis Pub/Sub转发；
     * Pub/Sub失败时同步写入离线库兜底，确保消息不丢。
     * 参数：
     *   toUserId  —— 目标用户ID
     *   msg       —— 消息JSON字符串
     *   route     —— 输出参数，记录实际路由路径（"local"/"redis"/"offline"）
     */
    void deliverMsg(int toUserId, const string &msg, string &route);

    // 存储消息id和其对应的业务处理方法
    unordered_map<int, MsgHandler> _msgHandlerMap;

    // 存储在线用户的通信连接
    unordered_map<int, TcpConnectionPtr> _userConnMap;

    // 定义互斥锁，保证 _userConnMap 的线程安全
    mutex _connMutex;

    // 数据操作类对象
    UserModel       _userModel;
    OfflineMsgModel _offlineMsgModel;
    FriendModel     _friendModel;
    GroupModel      _groupModel;

    // redis操作对象
    Redis _redis;

    // 【改动4】消息去重器：LRU+TTL，容量10000条，TTL120秒
    MsgDedup _dedup;
};

#endif
