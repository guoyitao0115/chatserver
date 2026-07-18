#include "chatserver.hpp"
#include "chatservice.hpp"
#include "framecodec.hpp"
#include "json.hpp"

#include <iostream>
#include <functional>
#include <string>
using namespace std;
using namespace placeholders;
using json = nlohmann::json;

// ============================================================
// 【改动1】ChatServer — 集成帧协议编解码器（FrameCodec）
// ============================================================
// 原来 onMessage 直接把 buffer->retrieveAllAsString() 当一条消息处理，
// 存在 TCP 粘包/拆包风险：一次 recv 可能收到多条消息，也可能只收到半条。
//
// 修改后：
//   - 构造 FrameCodec，传入"完整帧处理"回调 onFrameMessage
//   - onMessage 把 Buffer 直接交给 codec.decode() 拆帧
//   - codec 每拆出一个完整帧，就调用一次 onFrameMessage
//   - 发送时通过 FrameCodec::encode() 加上帧头再发送
//
// 帧结构：[4字节 payload_len（网络字节序）][payload(JSON字符串)]
// ============================================================

// 初始化聊天服务器对象
ChatServer::ChatServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg)
    : _server(loop, listenAddr, nameArg),
      _loop(loop),
      // 【改动1】初始化帧解码器，绑定完整帧到达后的处理回调
      _codec(std::bind(&ChatServer::onFrameMessage, this, _1, _2, _3))
{
    // 注册链接回调
    _server.setConnectionCallback(
        std::bind(&ChatServer::onConnection, this, _1));

    // 注册消息回调（muduo 每次 TCP 数据到达触发，可能粘包/拆包）
    // 【改动1】不再直接解析JSON，而是先交给 FrameCodec 拆帧
    _server.setMessageCallback(
        std::bind(&ChatServer::onMessage, this, _1, _2, _3));

    // 设置线程数量
    _server.setThreadNum(4);
}

// 启动服务
void ChatServer::start()
{
    _server.start();
}

// 上报链接相关信息的回调函数
void ChatServer::onConnection(const TcpConnectionPtr &conn)
{
    // 客户端断开链接
    if (!conn->connected())
    {
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

// ============================================================
// 【改动1】onMessage：TCP数据到达回调
// 直接将 muduo Buffer 交给 FrameCodec 进行拆帧处理。
// FrameCodec 内部循环解帧，每解出一个完整帧调用 onFrameMessage。
// 不完整的半帧数据留在 Buffer 中，等待下次数据到达再拼。
// ============================================================
void ChatServer::onMessage(const TcpConnectionPtr &conn,
                           Buffer *buffer,
                           Timestamp time)
{
    // 【改动1】交给帧解码器处理，解决粘包/拆包问题
    _codec.decode(conn, buffer, time);
}

// ============================================================
// 【改动1】onFrameMessage：完整帧到达回调（由 FrameCodec 调用）
// 此时 payload 已是完整的 JSON 字符串，可以安全解析。
// ============================================================
void ChatServer::onFrameMessage(const TcpConnectionPtr &conn,
                                const string &payload,
                                Timestamp time)
{
    try
    {
        // 数据的反序列化（此时 payload 一定是完整的单条消息）
        json js = json::parse(payload);

        // 达到的目的：完全解耦网络模块的代码和业务模块的代码
        // 通过 js["msgid"] 获取业务handler => conn js time
        auto msgHandler = ChatService::instance()->getHandler(
            js["msgid"].get<int>());

        // 回调消息绑定好的事件处理器，执行相应的业务处理
        msgHandler(conn, js, time);
    }
    catch (const json::exception &e)
    {
        // 【改动7】JSON解析异常打点，不崩溃服务
        LOG_ERROR << "[onFrameMessage] JSON parse error: " << e.what()
                  << " payload=" << payload.substr(0, 200);
    }
}
