# changes1.md — 针对用户提问的问题回答与代码修改说明

---

## 一、用户提问与回答

### 问题1：你把离线消息的 insert 从 void 改成 bool 有什么用呢，你又没处理 false 的情况

**回答：**
上一版确实存在此问题：offlinemessagemodel.cpp 的 insert 已返回 bool，
但 chatservice.cpp 中调用方 deliverMsg() 忽略了返回值，等同于仍然静默丢弃失败。
本次修复：
- deliverMsg() 改为返回 bool，内部检查 _offlineMsgModel.insert() 返回值。
- 落库失败时记录 LOG_ERROR 并向上返回 false。
- oneChat() / groupChat() 接收 deliverMsg() 返回值：
  投递失败时回 ACK_FAIL 而非 ACK_OK，发送端感知失败并可触发重试。

---

### 问题2：服务端发给客户端的消息不需要 ACK 吗，如果丢失了怎么办呢

**回答：需要，且这次已实现二级ACK（端到端送达确认）。**

TCP 本身保证字节传输可靠，但当接收端连接断开时在途消息会丢失。

两级ACK体系：
| 方向 | 类型 | 消息类型 |
|------|------|---------|
| 发送端->服务端 | 一级ACK：服务端已接收 | MSG_ACK |
| 接收端->服务端->发送端 | 二级ACK：接收端已展示 | MSG_DELIVER_ACK |

实现方式：
1. 接收端展示 ONE_CHAT_MSG/GROUP_CHAT_MSG 后立即向服务端发 MSG_DELIVER_ACK。
2. 服务端收到 MSG_DELIVER_ACK 后通过 deliverMsg() 转发给原发送端。
3. 原发送端在 readTaskHandler 中处理 MSG_DELIVER_ACK，打印"对方已收到"提示。
4. 接收端对展示做去重（markDisplayed()），防止重发导致重复展示。

---

### 问题3：消息没有收到 ACK 之后有实现重试机制吗，如何实现的

**回答：上一版只有 g_pendingMap 记录，但没有定时扫描重发逻辑，是个半成品。本次补全。**

实现原理：
1. 每条消息发送前生成唯一 message_id，加入 g_pendingMap（msgId->PendingEntry）。
2. PendingEntry 记录：原始payload、最后发送时间、已重试次数。
3. main() 启动后台重试线程 retryThreadHandler，每 500ms 扫描一次 g_pendingMap。
4. 超过等待窗口（指数退避）则重发；超过 MAX_RETRY(=3) 次后放弃并提示用户。
5. 收到 MSG_ACK(ACK_OK 或 ACK_DEDUP) 后调用 removePending() 停止重试。

指数退避策略：
  第1次重试: 等待 2000ms
  第2次重试: 等待 4000ms
  第3次重试: 等待 8000ms
  超过MAX_RETRY: 放弃，提示 message may be lost

---

### 问题4：服务端发送给客户端的消息并没有使用帧协议

**回答：确实是 Bug，本次修复。**

上一版问题：
- 客户端 readTaskHandler 使用 recvFrame()（先读4字节头）。
- 但服务端所有下行消息直接 conn->send(raw_json)，没有加帧头。
- 导致客户端 recvFrame() 把 JSON 数据前4字节当帧头，读出错误长度，流解析乱序崩溃。

修复方式：
- 在 chatservice.cpp 定义 sendJsonFrame(conn, js)：
  内部调用 FrameCodec::encode(js.dump()) 后发送，统一加帧头。
- 所有下行发送（login响应/reg响应/oneChat投递/groupChat投递/sendAck/handleDeliverAck）
  全部改为 sendJsonFrame() 或 FrameCodec::encode()。
- deliverMsg() 本地推送也改为 FrameCodec::encode(msg)。

---

## 二、修改的文件与代码说明

### 文件1：include/public.hpp

修改位置：第21行
新增枚举值：
```cpp
MSG_DELIVER_ACK,  // 二级ACK：接收端展示确认（接收端->服务端->发送端）
```
说明：新增消息类型，用于接收端向服务端回执"已展示"，服务端再转发给原发送端。

### 文件2：src/server/chatservice.cpp

#### 修改2-1：新增 sendJsonFrame 辅助函数（第11-14行）

解决问题4：所有服务端下行消息统一加帧头。

```cpp
// src/server/chatservice.cpp 第11~14行
static void sendJsonFrame(const TcpConnectionPtr &conn, const json &js)
{
    if (!conn || !conn->connected()) return;
    conn->send(FrameCodec::encode(js.dump())); // 统一加4字节帧头
}
```

#### 修改2-2：deliverMsg 返回 bool，本地推送加帧头（第64-90行）

同时解决问题1（insert false未处理）和问题4（本地推送未加帧头）。

```cpp
// src/server/chatservice.cpp 第64~90行
bool ChatService::deliverMsg(int toUserId, const string &msg, string &route)
{
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toUserId);
        if (it != _userConnMap.end())
        {
            it->second->send(FrameCodec::encode(msg)); // 问题4修复：加帧头
            route = "local"; return true;
        }
    }
    // redis publish ...
    bool ok = _offlineMsgModel.insert(toUserId, msg);
    if (!ok) {
        LOG_ERROR << "[deliverMsg] offline insert fail, to=" << toUserId;
        return false; // 问题1修复：落库失败向上返回
    }
    return true;
}
```

#### 修改2-3：oneChat/groupChat 根据 deliverMsg 返回值决定 ACK 状态

```cpp
// src/server/chatservice.cpp oneChat 约第222行
bool ok = deliverMsg(toId, js.dump(), route);
sendAck(conn, msgId, ok ? ACK_OK : ACK_FAIL); // 失败回 ACK_FAIL，触发客户端重试

// groupChat 约第252~256行
for (int id : users) {
    string route;
    if (!deliverMsg(id, js.dump(), route)) allOk = false;
}
sendAck(conn, msgId, allOk ? ACK_OK : ACK_FAIL);
```

#### 修改2-4：新增 handleDeliverAck（约第266~275行）

解决问题2：服务端收到二级ACK后转发给原发送端。

```cpp
// src/server/chatservice.cpp 第266~275行
void ChatService::handleDeliverAck(const TcpConnectionPtr &, json &js, Timestamp)
{
    int originSender = js["fromid"].get<int>();
    string route;
    bool ok = deliverMsg(originSender, js.dump(), route);
    LOG_INFO << "[deliverAck] message_id=" << js["message_id"].get<string>()
             << " to(sender)=" << originSender
             << " route=" << route << " ok=" << ok;
}
```

在构造函数中注册：
```cpp
// src/server/chatservice.cpp 第33行
_msgHandlerMap.insert({MSG_DELIVER_ACK, std::bind(&ChatService::handleDeliverAck, this, _1, _2, _3)});
```

### 文件3：src/client/main.cpp

#### 修改3-1：PendingEntry 结构体（第70~80行）

解决问题3：原来 PendingEntry 只有 payload，没有时间戳和重试计数。

```cpp
// src/client/main.cpp 第70~80行
struct PendingEntry {
    string payload;                                 // 待重发的完整JSON
    chrono::steady_clock::time_point lastSent;      // 最后一次发送时间
    int retryCount;                                 // 已重试次数
};
static map<string, PendingEntry> g_pendingMap;
static int g_clientFd = -1; // 全局fd供重试线程使用
```

#### 修改3-2：重试线程 retryThreadHandler（第95~125行）

解决问题3：实现指数退避重试，最多 MAX_RETRY=3 次。

```cpp
// src/client/main.cpp 第95~125行
static const int MAX_RETRY = 3;
static const int RETRY_BASE_MS = 2000;

static void retryThreadHandler()
{
    while (true) {
        this_thread::sleep_for(chrono::milliseconds(500)); // 每500ms扫描
        lock_guard<mutex> lk(g_pendingMutex);
        auto now = chrono::steady_clock::now();
        vector<string> toErase;
        for (auto &kv : g_pendingMap) {
            int waitMs = RETRY_BASE_MS * (1 << kv.second.retryCount); // 指数退避
            auto elapsed = duration_cast<milliseconds>(now-kv.second.lastSent).count();
            if (elapsed < waitMs) continue;
            if (kv.second.retryCount >= MAX_RETRY) {
                cerr << "[retry] " << kv.first << " FAILED, may be lost!" << endl;
                toErase.push_back(kv.first); continue;
            }
            kv.second.retryCount++;
            kv.second.lastSent = now;
            sendFrame(g_clientFd, kv.second.payload); // 重发
        }
        for (auto &id : toErase) g_pendingMap.erase(id);
    }
}
```

#### 修改3-3：main() 启动重试线程（第193~194行）

```cpp
// src/client/main.cpp 第193~194行
thread retryTask(retryThreadHandler);
retryTask.detach(); // 后台常驻
```

#### 修改3-4：readTaskHandler 中处理 MSG_DELIVER_ACK

解决问题2：接收端展示消息后发二级ACK。

注意：**展示去重（markDisplayed）已在后续审查中删除**，详见本文第四节。

```cpp
// src/client/main.cpp readTaskHandler 中
if (ONE_CHAT_MSG == msgtype) {
    string mid = js.contains("message_id") ? js["message_id"].get<string>() : "";
    cout << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
         << " said: " << js["msg"].get<string>() << endl;
    // 发送二级ACK
    if (!mid.empty()) {
        json ack;
        ack["msgid"] = MSG_DELIVER_ACK;
        ack["message_id"] = mid;
        ack["fromid"] = js["id"].get<int>();
        ack["toid"] = g_currentUser.getId();
        sendFrame(clientfd, ack.dump());
    }
}
// 收到二级ACK时（本机作为原发送端）：
if (MSG_DELIVER_ACK == msgtype) {
    cout << "[delivered] message_id=" << js["message_id"].get<string>()
         << " confirmed delivered to receiver" << endl;
}
```

---

## 三、修改文件汇总

| 文件 | 修改内容 | 解决问题 |
|------|---------|----------|
| include/public.hpp | 新增 MSG_DELIVER_ACK 枚举 | 问题2 |
| src/server/chatservice.cpp | sendJsonFrame/deliverMsg返回bool/handleDeliverAck | 问题1/2/4 |
| include/server/chatservice.hpp | deliverMsg返回bool/新增handleDeliverAck | 问题1/2 |
| src/client/main.cpp | PendingEntry+重试线程/MSG_DELIVER_ACK处理 | 问题2/3 |

---

## 四、后记：删除过度设计（展示去重 markDisplayed）

**背景：** 初版实现在客户端接收端加入了 `markDisplayed()` 函数，用于防止同一条消息被展示两次。

**问题：**
1. 服务端已有 `_dedup.isDuplicate()` 去重，同一 `message_id` 最多投递给接收端一次。
2. 服务端对接收端没有重推机制，接收端不存在收到重复消息的路径。
3. `markDisplayed()` 永远不会返回 `true`，是无效死代码。

**已从 src/client/main.cpp 删除：**
- `markDisplayed()` 函数及变量 `g_ddMutex`、`g_displayedIds`、`DD_MAX`
- `readTaskHandler` 中 `ONE_CHAT_MSG` / `GROUP_CHAT_MSG` 的调用
- `doLoginResponse` 中离线消息展示时的调用
- `#include <unordered_set>`

**结论：** 接收端展示去重只在服务端有重推机制时才有意义，详见 `problem.md` 问题2。
