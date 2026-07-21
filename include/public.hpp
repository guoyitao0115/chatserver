#ifndef PUBLIC_H
#define PUBLIC_H

/*
 * 客户端与服务端共享的应用层消息类型定义。
 *
 * 枚举值会直接出现在 JSON 的 msgid/ack_state 字段中，属于线上协议的一部分。
 * 已发布的枚举成员只能在末尾追加，不能重排或复用数值，否则旧客户端会把消息
 * 分派给错误的处理器。新增类型时还应同步检查客户端接收分支、服务端处理器注册
 * 以及 Web 前端的协议常量。
 *
 * ============================================================
 * 【改动2】新增消息类型：MSG_ACK（服务端接收确认）
 * ============================================================
 * 原有消息类型保持不变，值不变，向后兼容。
 * 新增：
 *   MSG_ACK        —— 服务端收到业务消息后返回的确认帧
 *
 * ACK JSON格式：
 *   {
 *     "msgid"     : MSG_ACK,
 *     "message_id": "<原消息的message_id>",
 *     "ack_state" : 0   // 0=成功 1=去重跳过 2=解析失败
 *   }
 *
 * 业务消息（ONE_CHAT_MSG / GROUP_CHAT_MSG）发送方需携带：
 *   "message_id": "<唯一ID>"  // 建议："<senderid>_<timestamp_ms>_<seq>"
 * ============================================================
 */
enum EnMsgType
{
    // 会话与账号：请求和响应使用独立 msgid，便于客户端等待特定响应。
    LOGIN_MSG = 1,      // 登录消息
    LOGIN_MSG_ACK,      // 登录响应消息
    LOGINOUT_MSG,       // 注销消息
    REG_MSG,            // 注册消息
    REG_MSG_ACK,        // 注册响应消息
    // 社交与聊天业务。一对一和群聊消息必须携带唯一 message_id 才能获得可靠去重。
    ONE_CHAT_MSG,       // 一对一聊天消息
    ADD_FRIEND_MSG,     // 添加好友消息

    // 群组管理与群发。
    CREATE_GROUP_MSG,   // 创建群组
    ADD_GROUP_MSG,      // 加入群组
    GROUP_CHAT_MSG,     // 群聊天

    // 可靠投递控制消息：确认的是“服务端已处理/已识别重复”，不代表对方已读。
    MSG_ACK,            // 服务端对业务消息的接收确认（ACK）

    // 应用层心跳用于维持 Redis 在线路由租约，并让服务端识别失活连接。
    HEARTBEAT_MSG,      // 客户端 -> 服务端 心跳请求
    HEARTBEAT_MSG_ACK,  // 服务端 -> 客户端 心跳应答
    ERROR_MSG,          // 服务端统一错误响应（认证失败、参数错误等）
};

/*
 * ACK 状态码，填入 MSG_ACK 的 ack_state 字段。
 * ACK_OK 与 ACK_DEDUP 都表示发送方无需继续重试；ACK_FAIL 表示本次处理未完成，
 * 客户端可在退避后使用同一个 message_id 重试，以继续享受幂等保护。
 */
enum AckState
{
    ACK_OK      = 0,   // 收到并处理成功
    ACK_DEDUP   = 1,   // 重复消息，已去重跳过
    ACK_FAIL    = 2,   // 解析或处理失败
};

#endif
