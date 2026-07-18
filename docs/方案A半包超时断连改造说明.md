# 方案A改造说明：半包超时断连（防止慢速半包长期占用资源）

> 改造日期：2026-03-31  
> 改造目标：当服务端读到消息头但长期收不齐消息体时，主动断开连接，避免连接和缓冲资源被恶意占用。

---

## 1. 改造背景

当前项目基于 `muduo + FrameCodec`，消息不完整时不会阻塞线程（这是正确的）。
但如果客户端恶意慢速发送（先发长度头，再长期不发完消息体），连接会长期处于“半包”状态，造成资源占用。

因此按方案A实现：

1. 识别连接是否进入“半包状态”
2. 记录半包首次出现时间
3. 定时巡检超时连接
4. 超时主动断连并清理状态

---

## 2. 修改文件与行号

### 文件一：`include/server/chatserver.hpp`

- `14` 行：新增头文件 `#include <unordered_map>`
- `49` 行：新增函数声明 `checkPartialFrameTimeouts()`
- `58` 行：新增半包超时阈值 `PARTIAL_FRAME_TIMEOUT_SEC = 10`
- `61` 行：新增半包状态表 `_partialFrameStart`

### 文件二：`src/server/chatserver.cpp`

- `11` 行：新增 `#include <cstring>`（用于 `memcpy`）
- `53` 行：构造函数里新增 `runEvery(1.0, ...)` 定时巡检
- `63-74` 行：`onConnection()` 中连接关闭时清理半包追踪状态
- `84-120` 行：`onMessage()` 中识别半包并记录/清理状态
- `152-183` 行：新增 `checkPartialFrameTimeouts()` 实现超时断连

---

## 3. 核心代码片段

### 3.1 头文件新增状态与接口（`chatserver.hpp`）

```cpp
// 半包超时巡检：超过阈值仍未收齐帧则断开连接
void checkPartialFrameTimeouts();

// 半包超时阈值（秒）
static constexpr int PARTIAL_FRAME_TIMEOUT_SEC = 10;

// 连接 -> 半包首次出现时间 + 连接弱引用（用于慢速半包超时断连）
unordered_map<string, pair<chrono::steady_clock::time_point, TcpConnectionWeakPtr>> _partialFrameStart;
mutex _partialFrameMutex;
```

### 3.2 定时巡检注册（`chatserver.cpp`）

```cpp
// 方案A：每秒巡检一次半包超时连接，超时则主动断开
_loop->runEvery(1.0, std::bind(&ChatServer::checkPartialFrameTimeouts, this));
```

### 3.3 在 `onMessage()` 中记录半包开始时间（`chatserver.cpp`）

```cpp
// 步骤1：处理前检查是否处于半包状态
bool hasPartialFrame = false;

// 先确认至少有4字节，才能安全读取长度头
if (buffer->readableBytes() >= static_cast<size_t>(FRAME_HEADER_LEN))
{
    uint32_t netLen = 0;
    // peek() 只看不消费，避免破坏后续 FrameCodec 解码
    memcpy(&netLen, buffer->peek(), FRAME_HEADER_LEN);

    // 网络字节序 -> 主机字节序，得到声明的payload长度
    uint32_t payloadLen = ntohl(netLen);

    // 若长度合法（不超过上限）且尚未收齐完整帧，则视为“半包”
    if (payloadLen <= FRAME_MAX_PAYLOAD &&
        buffer->readableBytes() < static_cast<size_t>(FRAME_HEADER_LEN + payloadLen))
    {
        hasPartialFrame = true;
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
hasPartialFrame = false;
if (buffer->readableBytes() >= static_cast<size_t>(FRAME_HEADER_LEN))
{
    uint32_t netLen = 0;
    memcpy(&netLen, buffer->peek(), FRAME_HEADER_LEN);
    uint32_t payloadLen = ntohl(netLen);

    if (payloadLen <= FRAME_MAX_PAYLOAD &&
        buffer->readableBytes() < static_cast<size_t>(FRAME_HEADER_LEN + payloadLen))
    {
        hasPartialFrame = true;
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
```

### 3.4 超时断连逻辑（`chatserver.cpp`）

```cpp
void ChatServer::checkPartialFrameTimeouts()
{
    vector<TcpConnectionPtr> toClose;
    auto now = chrono::steady_clock::now();

    {
        lock_guard<mutex> lock(_partialFrameMutex);
        for (auto it = _partialFrameStart.begin(); it != _partialFrameStart.end(); )
        {
            auto elapsed = chrono::duration_cast<chrono::seconds>(now - it->second.first).count();
            if (elapsed >= PARTIAL_FRAME_TIMEOUT_SEC)
            {
                TcpConnectionPtr conn = it->second.second.lock();
                if (conn)
                {
                    LOG_WARN << "[partial-frame-timeout] conn=" << it->first
                             << " timeout=" << elapsed << "s, close connection";
                    toClose.push_back(conn);
                }
                it = _partialFrameStart.erase(it);
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
```

---

## 4. 详细解释

### 4.1 为什么用“首次半包时间”而不是每次刷新时间

如果每次收到一点点数据都刷新超时计时，攻击者可以每隔几秒发 1 字节永久保活。  
当前实现只记录首次进入半包状态时间，不刷新；超过阈值直接断开，更能防慢速攻击。

### 4.2 为什么状态表保存弱引用 `TcpConnectionWeakPtr`

定时器线程（事件循环回调）巡检时，连接可能已经自然关闭。  
用弱引用 `lock()` 后再判断可避免持有无效连接对象，防止悬挂访问。

### 4.3 为什么在 `onConnection()` 和 `onMessage()` 都清理状态

- `onConnection()`：连接断开时兜底清理，防止状态泄漏。
- `onMessage()`：当半包已经被补齐并消费完，立即清理追踪，避免误判。

### 4.4 对现有业务的影响

- 不改变协议格式，不影响客户端正常通信。
- 只影响“长期不完整帧”的连接（默认 10 秒）。
- 正常网络抖动通常不会触发断连。

### 4.5 `onMessage()` 逐段白话解释（对应 `chatserver.cpp:84-158`）

这一段可以按 5 步理解：

1. **处理前检查半包状态**：`readableBytes >= 4` 时读取长度头，判断是否处于半包状态。
2. **记录或清理状态**：如果处于半包状态且首次记录，则记录开始时间；否则清理状态。
3. **解码处理**：调用 `FrameCodec` 处理数据，可能处理多个完整帧（粘包）。
4. **处理后重新检查**：解码后再次检查 buffer 状态，看是否进入新的半包状态。
5. **更新状态记录**：根据处理后的状态更新或清理半包记录。

关键点：
- 处理后重新检查确保粘包场景下的半包状态被正确识别。
- `peek()` 不会移动读指针，`FrameCodec` 后续还能完整处理。
- “只首次记录时间，不持续刷新”是抗慢速攻击的核心。

### 4.6 `checkPartialFrameTimeouts()` 逐段白话解释（对应 `chatserver.cpp:152-185`）

这一段是定时任务（每秒运行一次），逻辑如下：

1. **遍历半包追踪表**，计算每个连接已卡半包多久。
2. **超过阈值（10秒）就判定超时**。
3. **弱引用转强引用**：连接还活着才加入待关闭列表。
4. **立即从追踪表删除**：防止下轮重复处理。
5. **锁外统一 `shutdown()`**：降低锁持有时间，减少并发竞争。

为什么先收集再关闭：
- `shutdown()` 可能触发后续回调，属于相对重操作；
- 放在锁外做，避免拖慢其它线程访问 `_partialFrameStart`。

---

## 5. 验证建议

1. 正常客户端收发消息：应无行为变化。
2. 构造半包：只发 4 字节头，10 秒后应看到服务端 `partial-frame-timeout` 日志并断连。
3. 构造慢速 body：持续小流量补包但不在 10 秒内完成，仍应断连。
4. 压测多连接半包场景：观察连接数与内存占用是否可控。

---

## 6. 可调参数

当前阈值：`PARTIAL_FRAME_TIMEOUT_SEC = 10`（`chatserver.hpp` 第58行）

可按网络条件调整：
- 内网高质量环境：5~8 秒
- 跨地域网络：10~15 秒

建议后续把该值外置到配置文件/环境变量。
