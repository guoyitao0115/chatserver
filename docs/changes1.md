# ChatServer 问题讨论与修复记录

> 修改日期：2026-03-26  
> 基于版本：chatserver-master（已含 CHANGES.md 中的改动1~7）

---

## 一、你提出的问题与回答

### 问题1：insert 改成返回 bool 有什么用？你又没处理 false 的情况

**回答：**  
你说得完全正确。之前把 `OfflineMsgModel::insert` 的返回值从 `void` 改成 `bool`，只是"暴露了问题"，但调用方 `deliverMsg()` 里直接忽略了返回值，等于没有任何实际效果。

这是一个**不完整的改进**，应该在 `deliverMsg()` 里检查返回值：
- 落库成功：正常流程
- 落库失败：记录 `LOG_ERROR` 告警，明确提示"消息可能丢失"

**本次修复：** 在 `deliverMsg()` 中检查 `insert` 的返回值，失败时打印错误日志。

---

### 问题2：服务端发给客户端的消息不需要 ACK 吗？丢失了怎么办？

**回答：**  
这是一个**关键的可靠性漏洞**。当前系统只实现了"客户端→服务端"方向的 ACK，服务端发给客户端的响应（登录应答、注册应答、消息转发）没有任何确认机制。

从完整可靠性来看，应该有"服务端→客户端"方向的 ACK，但这需要大幅重构协议，引入**双向序列号 + 窗口机制**，工程量较大。

**当前项目的现实权衡：**
- 登录/注册响应丢失：客户端会超时重试整个登录/注册操作，代价可接受
- 消息转发丢失：对端用户收不到消息，但发送方已拿到服务端的 `ACK_OK`，认为投递成功

**真正解决这个问题需要：**
1. 接收方（被推送消息的客户端）收到消息后回 ACK 给服务端
2. 服务端未收到接收方 ACK 时重发
3. 引入消息序列号避免重复展示

**本次暂不实现双向 ACK**（工程量大，属于进阶优化），但在文档中明确记录这一问题。

---

### 问题3：没有收到 ACK 后有实现重试机制吗？如何实现的？

**回答：**  
改动2/3 里只实现了 `pending_map` 的框架（发送时加入、收到 ACK 后移除），但**没有实现超时重试定时器**，消息停留在 `pending_map` 里没有任何后续处理，等于没有重试。

**本次修复：** 在客户端新增一个**后台重试线程** `retryTaskHandler()`，每秒扫描 `pending_map`：
- 超过 5 秒未收到 ACK：重发消息，最多重试 3 次
- 超过最大重试次数：放弃，打印告警

---

## 二、本次代码修改内容

### 修改1：`deliverMsg()` 处理 insert 返回值
**文件：** `src/server/chatservice.cpp`  
**行号：** 第 144~152 行

**修改原因：** `_offlineMsgModel.insert()` 返回 bool 但之前被忽略，消息落库失败时无任何告警。

**修改前：**
```cpp
// --- 路径3 / 兜底：写入离线消息库 ---
_offlineMsgModel.insert(toUserId, msg);
```

**修改后：**
```cpp
// --- 路径3 / 兜底：写入离线消息库 ---
// 【改动8】检查离线消息落库是否成功，失败时记录告警
bool insertOk = _offlineMsgModel.insert(toUserId, msg);
if (!insertOk)
{
    LOG_ERROR << "[deliverMsg] offline message insert failed for userid=" << toUserId
              << ", message may be lost! route=" << route;
}
```

**解释：**  
- `insertOk` 接收 `insert()` 的 bool 返回值
- 如果返回 `false`，说明数据库连接失败或 SQL 执行失败，此时消息**真实丢失**
- 通过 `LOG_ERROR` 打印告警，包含用户 ID 和路由路径，方便排查
- 这使 `insert` 返回 `bool` 真正有了意义

---

### 修改2：`chatservice.cpp` 统一发送帧编码
**文件：** `src/server/chatservice.cpp`  
**行号：** 第 3 行（`#include "framecodec.hpp"`），第 94 行（sendAck），第 116 行（deliverMsg 本地推送），第 170/249/260/283/291 行（各响应发送点）

**修改原因：** undo 还原后发送端未加帧头，与客户端 `recvFrame()` 协议不一致。

**修改：** 所有 `conn->send(xxx.dump())` 统一改为 `conn->send(FrameCodec::encode(xxx.dump()))`

```cpp
// 第3行 - 新增 include
#include "framecodec.hpp"

// 第94行 - ACK 发送
conn->send(FrameCodec::encode(ack.dump()));

// 第116行 - 本地转发
it->second->send(FrameCodec::encode(msg));

// 各响应发送点
conn->send(FrameCodec::encode(response.dump()));
```

**解释：**  
帧结构为 `[4字节长度头][JSON payload]`，客户端 `recvFrame()` 先读 4 字节长度再读 payload。  
如果服务端发裸 JSON，客户端会把 JSON 的前 4 字节当长度解析，导致**消息解析完全错乱**。

---

### 修改3：`chatserver.hpp` 补充 TcpConnection.h
**文件：** `include/server/chatserver.hpp`  
**行号：** 第 15 行

**修改：**
```cpp
#include <muduo/net/TcpServer.h>
#include <muduo/net/TcpConnection.h>   // 新增，确保 TcpConnectionPtr 可见
#include <muduo/net/EventLoop.h>
```

**解释：**  
`TcpConnectionPtr` 是 `muduo::net::TcpConnection` 中定义的 `shared_ptr` 类型别名。  
`TcpServer.h` 不保证一定包含它，显式引入才能保证所有编译场景下类型可见。

---

### 修改4：客户端新增超时重试机制（核心新功能）
**文件：** `src/client/main.cpp`  

#### 4.1 新增全局变量
**行号：** 第 93~101 行

```cpp
// 【改动8】记录每条消息的发送时间戳，用于超时重试判断
static map<string, chrono::steady_clock::time_point> g_pendingTime;
// 超时重试间隔：5秒未收到ACK则重发
static const int RETRY_TIMEOUT_SEC = 5;
// 最大重试次数：超过后放弃并从pending移除
static const int MAX_RETRY_COUNT = 3;
static map<string, int> g_pendingRetry; // msgId -> 已重试次数
static int g_retryClientFd = -1;        // 重试线程使用的socket fd
```

**解释：**  
- `g_pendingTime`：记录每条消息的入队时间，用于计算是否超时
- `RETRY_TIMEOUT_SEC = 5`：5秒无 ACK 则触发重发
- `MAX_RETRY_COUNT = 3`：最多重试 3 次，避免无限发送
- `g_pendingRetry`：记录每条消息已重试的次数
- `g_retryClientFd`：重试线程持有的 fd，用于重发帧

#### 4.2 addPending / removePending 增加时间戳管理
**行号：** 第 103~117 行

```cpp
static void addPending(const string &msgId, const string &payload)
{
    lock_guard<mutex> lock(g_pendingMutex);
    g_pendingMap[msgId] = payload;
    g_pendingTime[msgId] = chrono::steady_clock::now(); // 记录入队时间
    g_pendingRetry[msgId] = 0;                          // 初始化重试计数
}

static void removePending(const string &msgId)
{
    lock_guard<mutex> lock(g_pendingMutex);
    g_pendingMap.erase(msgId);
    g_pendingTime.erase(msgId);   // 同步清理时间戳
    g_pendingRetry.erase(msgId);  // 同步清理重试计数
}
```

**解释：**  
- `addPending` 在消息发送时同时记录时间和重试计数
- `removePending` 在收到 ACK 时同时清理三个 map，避免内存泄漏

#### 4.3 重试线程 retryTaskHandler
**行号：** 第 119~175 行

```cpp
// 【改动8】重试线程：每秒扫描一次 pending_map，对超时未收到ACK的消息执行重发
static void retryTaskHandler()
{
    while (true)
    {
        this_thread::sleep_for(chrono::seconds(1));
        if (g_retryClientFd < 0) continue;

        vector<pair<string,string>> toRetry;  // 本轮需要重发的消息
        vector<string> toRemove;              // 本轮需要放弃的消息

        {
            lock_guard<mutex> lock(g_pendingMutex);
            auto now = chrono::steady_clock::now();
            for (auto &kv : g_pendingMap)
            {
                auto elapsed = chrono::duration_cast<chrono::seconds>(
                    now - g_pendingTime[kv.first]).count();
                if (elapsed >= RETRY_TIMEOUT_SEC)
                {
                    int &cnt = g_pendingRetry[kv.first];
                    if (cnt < MAX_RETRY_COUNT)
                    {
                        toRetry.push_back(kv);
                        cnt++;  // 增加重试计数
                        g_pendingTime[kv.first] = now; // 重置计时
                    }
                    else
                    {
                        toRemove.push_back(kv.first); // 超过最大次数，放弃
                    }
                }
            }
            // 清理放弃的消息
            for (auto &id : toRemove)
            {
                g_pendingMap.erase(id);
                g_pendingTime.erase(id);
                g_pendingRetry.erase(id);
            }
        }

        // 在锁外执行重发，避免持锁期间阻塞
        for (auto &kv : toRetry)
        {
            cerr << "[retry] resending message_id=" << kv.first
                 << " (retry " << g_pendingRetry[kv.first] << "/" << MAX_RETRY_COUNT << ")" << endl;
            sendFrame(g_retryClientFd, kv.second);
        }
        for (auto &id : toRemove)
        {
            cerr << "[retry] give up message_id=" << id
                 << ", max retry reached, message may be lost" << endl;
        }
    }
}
```

**解释：**  
- 每秒醒来一次，扫描 `g_pendingMap`
- 计算每条消息距上次发送的时间，超过 `RETRY_TIMEOUT_SEC(5秒)` 则重发
- 重发后更新时间戳，等待下一轮
- 超过 `MAX_RETRY_COUNT(3次)` 则从 pending 移除，打印放弃告警
- 重发在**锁外**执行，避免持锁时 `sendFrame` 阻塞导致死锁

#### 4.4 main 函数启动重试线程
**行号：** 第 221~224 行

```cpp
// 【改动8】启动重试线程：后台扫描 pending_map，对超时未收到ACK的消息重发
g_retryClientFd = clientfd;
std::thread retryTask(retryTaskHandler);
retryTask.detach();
```

**解释：**  
- 在建立连接后、进入主循环前启动重试线程
- `detach()` 让线程在后台独立运行，不阻塞主线程
- 先赋值 `g_retryClientFd` 再启动线程，保证线程内 fd 有效

---

## 三、修改文件与行号汇总

| 修改点 | 文件 | 行号 | 说明 |
|--------|------|------|------|
| 新增 `#include "framecodec.hpp"` | `src/server/chatservice.cpp` | 3 | 让 chatservice.cpp 使用帧编码 |
| `sendAck` 帧编码 | `src/server/chatservice.cpp` | 94 | ACK 消息加帧头发送 |
| `deliverMsg` 本地推送帧编码 | `src/server/chatservice.cpp` | 116 | 本地转发消息加帧头 |
| `insert` 返回值处理 | `src/server/chatservice.cpp` | 147~152 | 落库失败记录 LOG_ERROR |
| 各响应帧编码 | `src/server/chatservice.cpp` | 170/249/260/283/291 | 所有响应统一帧编码 |
| 补充 `TcpConnection.h` | `include/server/chatserver.hpp` | 15 | 修复 TcpConnectionPtr 不可见 |
| 新增重试全局变量 | `src/client/main.cpp` | 93~101 | 时间戳/重试计数/重试fd |
| `addPending` 增加时间戳 | `src/client/main.cpp` | 103~109 | 入队同时记录时间和重试计数 |
| `removePending` 同步清理 | `src/client/main.cpp` | 111~117 | 出队同时清理三个 map |
| 重试线程 `retryTaskHandler` | `src/client/main.cpp` | 121~175 | 超时重发 + 放弃机制 |
| 启动重试线程 | `src/client/main.cpp` | 222~224 | main 函数中 detach 重试线程 |

---

## 四、遗留问题说明

**问题2（服务端→客户端方向的 ACK）未实现的原因：**

完整的双向 ACK 需要：
1. 客户端收到服务端推送的聊天消息后，回一个 `CLIENT_MSG_ACK` 给服务端
2. 服务端维护"待确认消息队列"，超时后重推
3. 客户端展示层去重（避免重推导致消息重复显示）

这需要修改协议、服务端和客户端，工程量较大，属于**进阶可靠性改造**，当前版本暂不实现。  
当前的兜底策略是：若消息直发失败（TCP 断开），会走离线消息路径，用户下次登录可收到。