#ifndef PUBLIC_H
#define PUBLIC_H

/*
 * server和client的公共文件
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
    LOGIN_MSG = 1,      // 登录消息
    LOGIN_MSG_ACK,      // 登录响应消息
    LOGINOUT_MSG,       // 注销消息
    REG_MSG,            // 注册消息
    REG_MSG_ACK,        // 注册响应消息
    ONE_CHAT_MSG,       // 一对一聊天消息
    ADD_FRIEND_MSG,     // 添加好友消息

    CREATE_GROUP_MSG,   // 创建群组
    ADD_GROUP_MSG,      // 加入群组
    GROUP_CHAT_MSG,     // 群聊天

    MSG_ACK,            // 服务端对业务消息的接收确认（ACK）

    // ---- 应用层心跳 ----
    HEARTBEAT_MSG,      // 客户端 -> 服务端 心跳请求
    HEARTBEAT_MSG_ACK,  // 服务端 -> 客户端 心跳应答
    ERROR_MSG,          // 服务端统一错误响应（认证失败、参数错误等）
};

/*
 * ACK状态码枚举，填入MSG_ACK的 ack_state 字段
 */
enum AckState
{
    ACK_OK      = 0,   // 收到并处理成功
    ACK_DEDUP   = 1,   // 重复消息，已去重跳过
    ACK_FAIL    = 2,   // 解析或处理失败
};

#endif
