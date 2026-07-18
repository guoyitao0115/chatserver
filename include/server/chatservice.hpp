#ifndef CHATSERVICE_H
#define CHATSERVICE_H

/*
 * ChatService 业务层：
 * - 用户/好友/群组/离线消息管理
 * - RabbitMQ 跨节点转发
 * - 消息 ACK 与去重（去重仍用Redis键）
 */

#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include <mutex>
using namespace std;
using namespace muduo;
using namespace muduo::net;

#include "redis.hpp"
#include "rabbitmq_bus.hpp"
#include "groupmodel.hpp"
#include "friendmodel.hpp"
#include "usermodel.hpp"
#include "offlinemessagemodel.hpp"
#include "msgdedup.hpp"   // 消息去重模块
#include "json.hpp"
using json = nlohmann::json;

// Redis去重key前缀（跨实例共享）
static const string REDIS_DEDUP_KEY_PREFIX = "chat:dedup:";

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
    // 应用层心跳
    void heartbeat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void loginout(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理客户端异常退出
    void clientCloseException(const TcpConnectionPtr &conn);
    // 服务器异常，业务重置方法
    void reset();
    // 获取消息对应的处理器
    MsgHandler getHandler(int msgid);
    // 从RabbitMQ总线中接收跨节点消息（direct 精确路由）
    void handleRabbitMqBusMessage(int userid, string msg);

private:
    ChatService();

    /*
     * 向客户端发送服务端接收确认（MSG_ACK）
     * 参数：
     *   conn      —— 目标连接
     *   msgId     —— 原消息的 message_id
     *   ackState  —— ACK状态码（参见 public.hpp AckState 枚举）
     */
    void sendAck(const TcpConnectionPtr &conn, const string &msgId, int ackState);

    /*
     * 统一消息投递函数
     * 优先本节点直接推送，否则通过RabbitMQ按 serverId 精确路由跨节点转发；
     * 转发失败时同步写入离线库兜底，确保消息不丢。
     * 参数：
     *   toUserId  —— 目标用户ID
     *   msg       —— 消息JSON字符串
     *   route     —— 输出参数，记录实际路由路径（"local"/"rabbitmq"/"offline"）
     */
    bool deliverMsg(int toUserId, const string &msg, string &route);

    /*
     * 去重策略：优先Redis（跨实例/跨重启窗口），失败时回退本地LRU去重。
     * 返回 true=重复消息，false=新消息。
     */
    bool isDuplicateWithFallback(const string &msgId);
    void forgetMessageMark(const string &msgId);

    // 连接认证绑定：业务中的 id 只能等于该连接登录成功时绑定的用户。
    int authenticatedUserId(const TcpConnectionPtr &conn);
    bool requireAuthenticatedUser(const TcpConnectionPtr &conn,
                                  int claimedUserId,
                                  const string &action);
    void sendError(const TcpConnectionPtr &conn,
                   int code,
                   const string &message);

    // 存储消息id和其对应的业务处理方法
    unordered_map<int, MsgHandler> _msgHandlerMap;

    // 存储在线用户的通信连接
    unordered_map<int, TcpConnectionPtr> _userConnMap;

    // conn->name() -> userId，避免每条消息 O(n) 反查并阻止身份冒用。
    unordered_map<string, int> _connUserMap;

    // 定义互斥锁，保证 _userConnMap 的线程安全
    mutex _connMutex;

    // 数据操作类对象
    UserModel       _userModel;
    OfflineMsgModel _offlineMsgModel;
    FriendModel     _friendModel;
    GroupModel      _groupModel;

    // redis操作对象（仅用于去重键）
    Redis _redis;

    // RabbitMQ跨节点消息总线（direct 精确路由）
    RabbitMqBus _rabbitMqBus;

    // 当前服务实例ID（用于RabbitMQ routing_key与Redis在线路由）
    string _serverId = "server-1";

    // 消息去重器：LRU+TTL，容量10000条，TTL120秒
    MsgDedup _dedup;
};

#endif
