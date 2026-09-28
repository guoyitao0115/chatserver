# `chatserver.cpp` 讲解

## 作用概览

**网络层实现。** 接收 TCP 字节流、维护连接活动时间、处理长度帧、校验 JSON 基本结构并分发业务。这里解决的是粘包、半包、坏包和慢连接问题，不直接访问用户或消息表。

阅读位置：`src/server/chatserver.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-30 行

```cpp
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
```

这些头文件把“网络层实现”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 31-57 行

```cpp

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
```

这段为“网络层实现”准备常量、对象或生命周期收尾。准备阶段不产生业务成功承诺；只有后续连接、数据库或协议操作返回成功，状态才会对其他线程和客户端可见。

### 片段 3：第 58-79 行

```cpp

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
```

建立连接时记录最近活动时间；断开时删除所有巡检状态，并通知 ChatService 清理用户路由。网络层不知道用户 id，业务层通过反向连接索引完成身份清理。

互斥区保护 `共享状态` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

### 片段 4：第 80-101 行

```cpp
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
```

函数内静态对象把业务服务限制为进程内单例。所有网络回调共享同一份连接表和基础设施连接，避免不同 ChatServer 回调各自维护互相矛盾的在线状态。

每次收到字节先刷新活动时间，再让 FrameCodec 从同一个 Buffer 连续拆帧。若剩余字节构成半包，则记录半包开始时间；后续补齐时删除记录，长时间不补齐才会被巡检关闭。

### 片段 5：第 102-130 行

```cpp

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
```

读取先确认长度头完整，再判断正文是否已经收齐；不足时保留 Buffer 原状等待下一批字节。只有长度合法且正文完整才前移读指针，这同时处理了半包和一次到达多帧的粘包情况。

### 片段 6：第 131-152 行

```cpp
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
```

这段更新网络生命周期相关索引。用户到连接用于推送，连接到用户用于断线反查；活动时间和半包起点用于不同超时巡检。相应项必须在关闭路径同步删除，避免复用旧连接状态。

### 片段 7：第 153-174 行

```cpp
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
```

这段更新网络生命周期相关索引。用户到连接用于推送，连接到用户用于断线反查；活动时间和半包起点用于不同超时巡检。相应项必须在关闭路径同步删除，避免复用旧连接状态。 这里区分“本次解码后仍有半包”和“半包已补齐”：首次残留才记录起点，清空后立即删计时，防止完整连接被旧时间戳误关闭。

### 片段 8：第 175-197 行

```cpp

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
```

完整 payload 在这里解析为 JSON并检查整数 `msgid`。解析异常和字段错误统一编码为 ERROR 帧返回；合法消息通过 `msgid` 找到 ChatService 处理器，从而保持网络拆包与业务逻辑分离。

写出数据前补上 4 字节大端长度头，接收方因此能从连续 TCP 字节中判断一条 JSON 到哪里结束。例如两条消息合并到一次读取时，会按各自长度连续拆出，而不会把两个 JSON 拼成坏数据。

### 片段 9：第 198-223 行

```cpp

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
```

巡检先在锁内收集超过空闲阈值的连接，解锁后再 shutdown。`toClose` 是本轮“待关闭连接”的局部 `vector<TcpConnectionPtr>`，不是关闭动作本身：`push_back` 复制强引用，确保从活动表删掉 weak_ptr 后对象仍存活到后面的循环。分两阶段让锁内只做判断、升级 weak_ptr 和 `erase`（避免下轮重复选中）；`shutdown()` 可能投递到连接所属子 Reactor 并最终触发关闭生命周期，放在锁外可避免长时间占用或重入活动表锁。

### 片段 10：第 224-245 行

```cpp

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
```

这段更新网络生命周期相关索引。用户到连接用于推送，连接到用户用于断线反查；活动时间和半包起点用于不同超时巡检。相应项必须在关闭路径同步删除，避免复用旧连接状态。 巡检先在锁内筛选超时连接并升级 weak_ptr，实际 shutdown 放到锁外，避免关闭回调重入活动表锁。

### 片段 11：第 246-269 行

```cpp
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
```

半包计时只针对 Buffer 中尚未组成完整帧的尾部。超过阈值说明客户端长期占着连接只发部分数据，关闭它可防慢速发送攻击和无界连接占用。

### 片段 12：第 270-298 行

```cpp

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
```

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

## 面试重点

- TCP 为什么必须自行处理半包和粘包，4 字节长度头如何完成增量解码？

- WebSocket 帧与后端 TCP 长度帧的边界分别在哪里，网关为什么不能承担最终鉴权？

- 最大帧长、半包超时和空闲超时各自防什么问题？
