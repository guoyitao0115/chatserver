#include "chatserver.hpp"
#include "chatservice.hpp"
#include "framecodec.hpp"
#include "public.hpp"
#include "json.hpp"

#include <iostream>
#include <functional>
#include <string>
#include <chrono>
#include <vector>
#include <cstring>
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

    // 方案A：每秒巡检一次半包超时连接，超时则主动断开
    _loop->runEvery(1.0, std::bind(&ChatServer::checkPartialFrameTimeouts, this));
    // 服务端主动检测静默连接；正常客户端每10秒心跳，45秒无流量才断开。
    _loop->runEvery(5.0, std::bind(&ChatServer::checkIdleConnections, this));
}

// 启动服务
void ChatServer::start()
{
    _server.start();
}

// 上报链接相关信息的回调函数
void ChatServer::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        lock_guard<mutex> lock(_activityMutex);
        _lastActivity[conn->name()] = {chrono::steady_clock::now(), conn};
        return;
    }

    // 连接关闭时清理半包追踪状态
    {
        lock_guard<mutex> lock(_partialFrameMutex);
        _partialFrameStart.erase(conn->name());
    }
    {
        lock_guard<mutex> lock(_activityMutex);
        _lastActivity.erase(conn->name());
    }

    ChatService::instance()->clientCloseException(conn);
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
    {
        lock_guard<mutex> lock(_activityMutex);
        _lastActivity[conn->name()] = {chrono::steady_clock::now(), conn};
    }

    // 步骤1：处理前检查是否处于半包状态
    bool hasPartialFrame = buffer->readableBytes() > 0;

    // 先确认至少有4字节，才能安全读取长度头
    if (buffer->readableBytes() >= static_cast<size_t>(FRAME_HEADER_LEN))
    {
        uint32_t payloadLen = chatserver::protocol::decodeFrameLength(buffer->peek());

        // 若长度合法（不超过上限）且尚未收齐完整帧，则视为“半包”
        if (chatserver::protocol::isValidPayloadLength(payloadLen) &&
            buffer->readableBytes() < static_cast<size_t>(FRAME_HEADER_LEN + payloadLen))
        {
            hasPartialFrame = true;
        }
        else
        {
            hasPartialFrame = false;
        }
    }

    // 步骤2：记录或清理半包状态
    if (hasPartialFrame)
    {
        lock_guard<mutex> lock(_partialFrameMutex);
        if (_partialFrameStart.find(conn->name()) == _partialFrameStart.end())
        {
            _partialFrameStart[conn->name()] = {chrono::steady_clock::now(), conn};
        }
    }
    else
    {
        // 如果当前不处于半包状态，清理之前的记录
        lock_guard<mutex> lock(_partialFrameMutex);
        _partialFrameStart.erase(conn->name());
    }

    // 步骤3：正常交给帧解码器处理（拆包/粘包）
    _codec.decode(conn, buffer, time);

    // 步骤4：处理后重新检查是否进入半包状态
    hasPartialFrame = buffer->readableBytes() > 0;
    if (buffer->readableBytes() >= static_cast<size_t>(FRAME_HEADER_LEN))
    {
        uint32_t payloadLen = chatserver::protocol::decodeFrameLength(buffer->peek());

        if (chatserver::protocol::isValidPayloadLength(payloadLen) &&
            buffer->readableBytes() < static_cast<size_t>(FRAME_HEADER_LEN + payloadLen))
        {
            hasPartialFrame = true;
        }
        else
        {
            hasPartialFrame = false;
        }
    }

    // 步骤5：根据处理后的状态更新半包记录
    if (hasPartialFrame)
    {
        lock_guard<mutex> lock(_partialFrameMutex);
        if (_partialFrameStart.find(conn->name()) == _partialFrameStart.end())
        {
            _partialFrameStart[conn->name()] = {chrono::steady_clock::now(), conn};
        }
    }
    else
    {
        // 如果处理后不处于半包状态，清理记录
        lock_guard<mutex> lock(_partialFrameMutex);
        _partialFrameStart.erase(conn->name());
    }
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

        if (!js.is_object() || !js.contains("msgid") || !js["msgid"].is_number_integer())
        {
            json error;
            error["msgid"] = ERROR_MSG;
            error["code"] = 400;
            error["message"] = "msgid must be an integer";
            conn->send(FrameCodec::encode(error.dump()));
            return;
        }

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
        json error;
        error["msgid"] = ERROR_MSG;
        error["code"] = 400;
        error["message"] = "invalid json payload";
        conn->send(FrameCodec::encode(error.dump()));
    }
}

void ChatServer::checkIdleConnections()
{
    vector<TcpConnectionPtr> toClose;
    const auto now = chrono::steady_clock::now();

    {
        lock_guard<mutex> lock(_activityMutex);
        for (auto it = _lastActivity.begin(); it != _lastActivity.end(); )
        {
            const auto idleSeconds = chrono::duration_cast<chrono::seconds>(
                now - it->second.first).count();
            if (idleSeconds >= IDLE_CONNECTION_TIMEOUT_SEC)
            {
                TcpConnectionPtr conn = it->second.second.lock();
                if (conn)
                {
                    LOG_WARN << "[idle-timeout] conn=" << it->first
                             << " idle=" << idleSeconds << "s, close connection";
                    toClose.push_back(conn);
                }
                it = _lastActivity.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto &conn : toClose)
    {
        conn->shutdown();
    }
}

// 方案A：巡检半包状态，超过阈值仍未收齐则主动断开
void ChatServer::checkPartialFrameTimeouts()
{
    // 先收集需要关闭的连接，避免持锁状态下直接做 shutdown（减少锁持有时间）
    vector<TcpConnectionPtr> toClose;
    auto now = chrono::steady_clock::now();

    {
        lock_guard<mutex> lock(_partialFrameMutex);

        // 遍历所有“处于半包中的连接”
        for (auto it = _partialFrameStart.begin(); it != _partialFrameStart.end(); )
        {
            // 计算该连接进入半包状态已持续多久
            auto elapsed = chrono::duration_cast<chrono::seconds>(now - it->second.first).count();

            // 超过阈值：判定为慢速半包或异常连接，准备断开
            if (elapsed >= PARTIAL_FRAME_TIMEOUT_SEC)
            {
                // 先从弱引用提升为强引用；若失败表示连接已不存在
                TcpConnectionPtr conn = it->second.second.lock();
                if (conn)
                {
                    LOG_WARN << "[partial-frame-timeout] conn=" << it->first
                             << " timeout=" << elapsed << "s, close connection";
                    toClose.push_back(conn);
                }

                // 无论连接是否还在，都要从追踪表移除，避免重复处理
                it = _partialFrameStart.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // 锁外执行真正断连，避免阻塞其他线程对状态表的访问
    for (auto &conn : toClose)
    {
        conn->shutdown();
    }
}
