# `main.cpp` 讲解

## 作用概览

**C++ 命令行客户端。** 实现长度帧收发、登录注册、聊天命令、心跳、ACK 重试和按会话顺序展示。多个后台线程共享 socket 与待确认表，因此写锁和条件变量决定其可靠性。

阅读位置：`src/client/main.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-26 行

```cpp
#include "json.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>
#include <algorithm>
#include <cstdint>
#include <tuple>
#include <cerrno>
#include <csignal>
using namespace std;
using json = nlohmann::json;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
```

这些头文件把“C++ 命令行客户端”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 27-48 行

```cpp

#include "group.hpp"
#include "user.hpp"
#include "public.hpp"
#include "frameprotocol.hpp"
#include "msgdedup.hpp"

// ============================================================
// 帧协议辅助函数（客户端版）
// 帧结构：[4字节 payload_len（网络字节序大端）][payload(JSON字符串)]
// ============================================================

// 心跳、重试线程和交互线程共享一个 socket；整帧发送必须串行化。
static mutex g_sendMutex;

// 将 JSON payload 打包成帧发送，返回发送字节数，-1表示失败
static int sendFrame(int fd, const string &payload)
{
    string frame;
    try
    {
        frame = chatserver::protocol::encodeFrame(payload);
    }
```

`msgdedup.hpp` 原本用于服务端 Redis 故障回退，现在也复用于 CLI 最终展示去重。发送
端仍先生成 4 字节长度头，再在写锁下循环 `send`，保证心跳、重试和前台命令不会把
各自字节交叉到同一 TCP 流。

### 片段 3：第 49-76 行

```cpp
    catch (const length_error &)
    {
        return -1;
    }

    lock_guard<mutex> lock(g_sendMutex);
    size_t total = frame.size(), sent = 0;
    while (sent < total)
    {
        ssize_t n = ::send(fd, frame.data() + sent, total - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += n;
    }
    return (int)sent;
}

// 精确读取 n 字节
static bool recvAll(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        got += r;
    }
```

互斥区保护 `共享状态` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 4：第 77-104 行

```cpp
    return true;
}

// 接收一个完整帧，返回 payload 字符串，失败返回空串
static string recvFrame(int fd)
{
    char header[4] = {0};
    if (!recvAll(fd, header, 4)) return "";
    uint32_t payloadLen = chatserver::protocol::decodeFrameLength(header);
    if (!chatserver::protocol::isValidPayloadLength(payloadLen)) return "";
    string payload(payloadLen, '\0');
    if (!recvAll(fd, &payload[0], payloadLen)) return "";
    return payload;
}

// ============================================================
// 消息ID生成与 pending 管理
// 每条业务消息携带唯一 message_id，收到服务端 MSG_ACK 后移出 pending
// ============================================================
static atomic_int g_myUserId{0};

// Snowflake 64位ID生成器：
// 1位符号位(固定0) + 41位时间戳差值 + 10位workerId + 12位毫秒内序列
class SnowflakeIdGenerator
{
public:
    explicit SnowflakeIdGenerator(uint16_t workerId = 1)
        : _workerId(workerId & WORKER_ID_MASK) {}
```

接收端先精确读取 4 字节头，校验长度后再精确读取 payload。网络一次 `recv` 可能只返回一部分，所以底层循环是避免把半包误当完整 JSON 的关键。

消息 id 生成器把毫秒时间、客户端 workerId 和同毫秒序号组合成唯一值；时钟未前进时递增序号，序号耗尽便等待下一毫秒。转换为字符串后，它既可作为 pending 表键，也可跨 Redis 节点去重。

### 片段 5：第 105-129 行

```cpp

    void setWorkerId(uint16_t workerId)
    {
        lock_guard<mutex> lock(_mtx);
        _workerId = workerId & WORKER_ID_MASK;
    }

    uint64_t nextId()
    {
        lock_guard<mutex> lock(_mtx);

        uint64_t ts = nowMs();
        if (ts < _lastTs)
        {
            // 时钟回拨保护：回拨窗口内强制使用 lastTs，保证单调不倒退
            ts = _lastTs;
        }

        if (ts == _lastTs)
        {
            _seq = (_seq + 1) & SEQ_MASK;
            if (_seq == 0)
            {
                ts = waitNextMs(_lastTs);
            }
```

这部分完成“C++ 命令行客户端”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。

### 片段 6：第 130-157 行

```cpp
        }
        else
        {
            _seq = 0;
        }

        _lastTs = ts;

        uint64_t id = ((ts - EPOCH_MS) << TIMESTAMP_SHIFT)
                    | (static_cast<uint64_t>(_workerId) << WORKER_ID_SHIFT)
                    | _seq;
        return id;
    }

private:
    static uint64_t nowMs()
    {
        return chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
    }

    static uint64_t waitNextMs(uint64_t lastTs)
    {
        uint64_t ts = nowMs();
        while (ts <= lastTs)
        {
            ts = nowMs();
        }
```

这一接口片段规定“C++ 命令行客户端”对外可用的操作和对象必须长期保存的状态。调用者只依赖这里的契约；锁、SQL、网络错误和资源释放留在实现内部，因此更换基础设施不会迫使业务处理器改写所有调用点。

### 片段 7：第 158-183 行

```cpp
        return ts;
    }

private:
    static constexpr uint64_t EPOCH_MS = 1704067200000ULL; // 2024-01-01 00:00:00 UTC
    static constexpr uint64_t WORKER_ID_BITS = 10;
    static constexpr uint64_t SEQ_BITS = 12;
    static constexpr uint64_t WORKER_ID_MASK = (1ULL << WORKER_ID_BITS) - 1;
    static constexpr uint64_t SEQ_MASK = (1ULL << SEQ_BITS) - 1;
    static constexpr uint64_t WORKER_ID_SHIFT = SEQ_BITS;
    static constexpr uint64_t TIMESTAMP_SHIFT = WORKER_ID_BITS + SEQ_BITS;

    mutex _mtx;
    uint16_t _workerId = 1;
    uint64_t _lastTs = 0;
    uint64_t _seq = 0;
};

static SnowflakeIdGenerator g_snowflake(1);

// 发送端序列号：
// - 单聊：每个接收者(toid)独立递增
// - 群聊：每个群组(groupid)独立递增
static mutex g_clientSeqMutex;
static unordered_map<int, uint64_t> g_peerClientSeq;
static unordered_map<int, uint64_t> g_groupClientSeq;
```

这一接口片段规定“C++ 命令行客户端”对外可用的操作和对象必须长期保存的状态。调用者只依赖这里的契约；锁、SQL、网络错误和资源释放留在实现内部，因此更换基础设施不会迫使业务处理器改写所有调用点。 这里固定雪花 ID 的纪元和位宽，决定时间、worker 与同毫秒序号如何拼接，也决定可支持的节点数和每毫秒上限。

### 片段 8：第 184-207 行

```cpp

static uint64_t nextClientSeqForPeer(int toid)
{
    lock_guard<mutex> lock(g_clientSeqMutex);
    uint64_t &seq = g_peerClientSeq[toid];
    seq += 1;
    return seq;
}

static uint64_t nextClientSeqForGroup(int groupid)
{
    lock_guard<mutex> lock(g_clientSeqMutex);
    uint64_t &seq = g_groupClientSeq[groupid];
    seq += 1;
    return seq;
}

static string generateMsgId()
{
    return to_string(g_snowflake.nextId());
}

static mutex g_pendingMutex;
static map<string, string> g_pendingMap; // msgId -> payload
```

超时扫描只在锁内决定哪些消息重发或放弃，并同步更新计时与次数；真正写 socket 在锁外执行。达到上限的 id 从全部 pending 表删除并单独告警，防止同一失败项永久占用内存和反复发送。

### 片段 9：第 208-236 行

```cpp

// 记录每条消息的发送时间戳，用于超时重试判断
static map<string, chrono::steady_clock::time_point> g_pendingTime;
// 超时重试间隔：5秒未收到ACK则重发
static const int RETRY_TIMEOUT_SEC = 5;
// 最大重试次数：超过后放弃并从pending移除
static const int MAX_RETRY_COUNT = 3;

// 应用层心跳间隔（秒）：定时向服务端发送 HEARTBEAT_MSG
static const int HEARTBEAT_INTERVAL_SEC = 10;
static atomic_bool g_heartbeatAcked{true};
static map<string, int> g_pendingRetry; // msgId -> 已重试次数
static int g_retryClientFd = -1; // 重试线程使用的socket fd

static void addPending(const string &msgId, const string &payload)
{
    lock_guard<mutex> lock(g_pendingMutex);
    g_pendingMap[msgId] = payload;
    g_pendingTime[msgId] = chrono::steady_clock::now();
    g_pendingRetry[msgId] = 0;
}

static void removePending(const string &msgId)
{
    lock_guard<mutex> lock(g_pendingMutex);
    g_pendingMap.erase(msgId);
    g_pendingTime.erase(msgId);
    g_pendingRetry.erase(msgId);
}
```

三张 pending 状态以同一个 message_id 对齐：保存原 payload 才能原样重发，重试次数限制无限发送，首次/上次时间决定何时重试。ACK 到达时必须同时删除这些记录，否则后台线程还会发送已经成功的消息。

### 片段 10：第 237-266 行

```cpp

// 重试线程：每秒扫描一次 pending_map，对超时未收到ACK的消息执行重发
// 超过 MAX_RETRY_COUNT 次后放弃，打印告警
static void retryTaskHandler()
{
    while (true)
    {
        this_thread::sleep_for(chrono::seconds(1));
        if (g_retryClientFd < 0) continue;

        vector<tuple<string,string,int>> toRetry;
        vector<string> toRemove;

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
                        int nextRetry = ++cnt;
                        toRetry.emplace_back(kv.first, kv.second, nextRetry);
                        // 重置计时，等待下一轮ACK
                        g_pendingTime[kv.first] = now;
                    }
```

线程定期扫描 pending 表：超过 ACK 等待时间的消息增加重试次数并重新发送，达到上限则移除并提示失败。扫描时只整理待发送副本，真正 socket 写在锁外完成，降低对前台发送的阻塞。

### 片段 11：第 267-290 行

```cpp
                    else
                    {
                        // 超过最大重试次数，放弃
                        toRemove.push_back(kv.first);
                    }
                }
            }
            for (auto &id : toRemove)
            {
                g_pendingMap.erase(id);
                g_pendingTime.erase(id);
                g_pendingRetry.erase(id);
            }
        }

        for (auto &kv : toRetry)
        {
            const string &msgId = get<0>(kv);
            const string &payload = get<1>(kv);
            int retryCnt = get<2>(kv);
            cerr << "[retry] resending message_id=" << msgId
                 << " (retry " << retryCnt << "/" << MAX_RETRY_COUNT << ")" << endl;
            sendFrame(g_retryClientFd, payload);
        }
```

超时扫描只在锁内决定哪些消息重发或放弃，并同步更新计时与次数；真正写 socket 在锁外执行。达到上限的 id 从全部 pending 表删除并单独告警，防止同一失败项永久占用内存和反复发送。 这里执行扫描结果：先从三张待确认表同步删除放弃项，再在锁外发送重试副本，最后逐条打印达到上限的 message_id。

### 片段 12：第 291-316 行

```cpp
        for (auto &id : toRemove)
        {
            cerr << "[retry] give up message_id=" << id
                 << ", max retry reached, message may be lost" << endl;
        }
    }
}

// 心跳线程：周期发送 HEARTBEAT_MSG，若连续未收到ACK可快速识别异常连接
static void heartbeatTaskHandler(int clientfd)
{
    while (true)
    {
        this_thread::sleep_for(chrono::seconds(HEARTBEAT_INTERVAL_SEC));

        // 仅登录后发送心跳，避免登录前无意义探测
        if (g_myUserId.load() <= 0)
        {
            continue;
        }

        json hb;
        hb["msgid"] = HEARTBEAT_MSG;
        hb["id"] = g_myUserId.load();
        hb["ts"] = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
```

超时扫描只在锁内决定哪些消息重发或放弃，并同步更新计时与次数；真正写 socket 在锁外执行。达到上限的 id 从全部 pending 表删除并单独告警，防止同一失败项永久占用内存和反复发送。 这里执行扫描结果：先从三张待确认表同步删除放弃项，再在锁外发送重试副本，最后逐条打印达到上限的 message_id。 源码在这一段特别限定了“心跳线程：周期发送 HEARTBEAT_MSG，若连续未收到ACK可快速识别异常连接”，因此解释范围止于该局部步骤。

### 片段 13：第 317-342 行

```cpp

        // 上一轮心跳若还未被ACK，打印告警（不立刻断线，避免误判）
        if (!g_heartbeatAcked.load())
        {
            cerr << "[heartbeat] previous heartbeat not acked yet, connection may be unstable" << endl;
        }

        g_heartbeatAcked = false;
        if (sendFrame(clientfd, hb.dump()) == -1)
        {
            cerr << "[heartbeat] send heartbeat failed" << endl;
        }
    }
}

// ============================================================
// 原有全局状态
// ============================================================
User g_currentUser;
vector<User> g_currentUserFriendList;
vector<Group> g_currentUserGroupList;
atomic_bool isMainMenuRunning{false};
atomic_bool g_isLoginSuccess{false};
static mutex g_responseMutex;
static condition_variable g_responseCv;
static uint64_t g_responseVersion = 0;
```

登录后才周期发送心跳。发送下一轮前若上一轮仍未确认，只提示连接可能不稳定而不立即断开，减少短暂调度延迟造成误判；收到 HEARTBEAT_ACK 后接收线程会重新设置确认标志。

### 片段 14：第 343-376 行

```cpp

static uint64_t responseVersion()
{
    lock_guard<mutex> lock(g_responseMutex);
    return g_responseVersion;
}

static void waitForResponse(uint64_t previousVersion)
{
    unique_lock<mutex> lock(g_responseMutex);
    g_responseCv.wait(lock, [previousVersion] { return g_responseVersion > previousVersion; });
}

static void notifyResponse()
{
    {
        lock_guard<mutex> lock(g_responseMutex);
        ++g_responseVersion;
    }
    g_responseCv.notify_one();
}

// 每个会话（单聊按发送者id，群聊按groupid+发送者id）最后已显示的 client_seq
static unordered_map<string, uint64_t> g_lastShownSeq;
// 乱序缓冲：key=session, value=(seq -> message)
struct BufferedMessage
{
    string line;
    chrono::steady_clock::time_point receivedAt;
};
static unordered_map<string, map<uint64_t, BufferedMessage>> g_orderBuffer;
static mutex g_orderMutex;
static constexpr int ORDER_GAP_TIMEOUT_MS = 2000;
static constexpr size_t ORDER_BUFFER_MAX_PER_SESSION = 100;
// RabbitMQ manual ACK 重投、publisher confirm 超时后的离线兜底以及离线消息重放，
// 都可能让同一逻辑消息沿不同路径到达客户端。最终展示前再按 message_id 去重。
static MsgDedup g_receivedMsgDedup(50000, 24 * 60 * 60);
```

顺序缓冲解决 `client_seq` 缺口，`g_receivedMsgDedup` 解决同一逻辑消息的多副本，两者
职责不同。接收去重保存最多 50,000 个 ID、有效期 24 小时，避免集合无限增长。

### 片段 15：第 377-401 行

```cpp

static string buildSessionKey(const json &js)
{
    if (ONE_CHAT_MSG == js["msgid"].get<int>())
    {
        // 单聊：按发送者维度重排
        return string("u:") + to_string(js["id"].get<int>());
    }

    // 群聊：按“群+发送者”维度重排，避免不同发送者序号混用
    return string("g:") + to_string(js["groupid"].get<int>()) +
           ":u:" + to_string(js["id"].get<int>());
}

static string formatIncomingLine(const json &js)
{
    if (ONE_CHAT_MSG == js["msgid"].get<int>())
    {
        return js["time"].get<string>() + " [" + to_string(js["id"].get<int>()) + "]" +
               js["name"].get<string>() + " said: " + js["msg"].get<string>();
    }
    return string("群消息[") + to_string(js["groupid"].get<int>()) + "]:" +
           js["time"].get<string>() + " [" + to_string(js["id"].get<int>()) + "]" +
           js["name"].get<string>() + " said: " + js["msg"].get<string>();
}
```

这一组值把部署差异留在环境层：服务进程和 Compose 使用同名键，测试还可临时调大消息数或超时。示例文件只给安全占位和本机默认，不应保存真实生产密码。

### 片段 16：第 402-433 行

```cpp

static void printOrderedIncoming(const json &js)
{
    if (js.contains("message_id") && js["message_id"].is_string())
    {
        const string messageId = js["message_id"].get<string>();
        if (g_receivedMsgDedup.isDuplicate(messageId))
        {
            cerr << "[dedup] skip duplicate received message_id="
                 << messageId << endl;
            return;
        }
    }

    if (!js.contains("client_seq"))
    {
        cout << formatIncomingLine(js) << endl;
        return;
    }

    string key = buildSessionKey(js);
    uint64_t seq = js["client_seq"].get<uint64_t>();
    lock_guard<mutex> lock(g_orderMutex);
    uint64_t &last = g_lastShownSeq[key];

    if (seq <= last)
    {
        return; // 重复/过期消息
    }

    g_orderBuffer[key][seq] = {formatIncomingLine(js), chrono::steady_clock::now()};

    // 连续可显示的序号依次输出
    auto &buf = g_orderBuffer[key];
    while (!buf.empty())
    {
        auto it = buf.begin();
        if (it->first == last + 1)
        {
            cout << it->second.line << endl;
            last = it->first;
            buf.erase(it);
        }
```

消息进入顺序缓冲前先按 `message_id` 去重，因此 online、RabbitMQ 重投和 offline replay
即使携带相同 `client_seq` 或没有序号，也只会展示一次。之后才按“会话 + 发送者”
维护下一期望序号，避免把消息 ID 幂等和展示顺序混成同一种机制。

### 片段 17：第 434-465 行

```cpp
        else
        {
            break;
        }
    }
}

// 序号洞超过 2 秒后跳过缺失区间，避免后续消息永久队头阻塞。
static void orderFlushTaskHandler()
{
    while (true)
    {
        this_thread::sleep_for(chrono::milliseconds(200));
        lock_guard<mutex> lock(g_orderMutex);
        const auto now = chrono::steady_clock::now();

        for (auto &session : g_orderBuffer)
        {
            auto &buf = session.second;
            uint64_t &last = g_lastShownSeq[session.first];
            if (buf.empty()) continue;

            auto first = buf.begin();
            const auto waited = chrono::duration_cast<chrono::milliseconds>(
                now - first->second.receivedAt).count();
            if (first->first > last + 1 &&
                (waited >= ORDER_GAP_TIMEOUT_MS || buf.size() >= ORDER_BUFFER_MAX_PER_SESSION))
            {
                cerr << "[order] missing seq " << (last + 1) << ".." << (first->first - 1)
                     << ", flush buffered messages after timeout" << endl;
                last = first->first - 1;
            }
```

每个会话的顺序状态记录下一期望序号、暂存的较新消息和序号洞开始时间。例如先到 seq=5、后到 seq=4 时先缓存 5，输出 4 后立即连续输出 5；若 4 永不出现，超时冲刷避免界面永久阻塞。

### 片段 18：第 466-494 行

```cpp

            while (!buf.empty() && buf.begin()->first == last + 1)
            {
                auto it = buf.begin();
                cout << it->second.line << endl;
                last = it->first;
                buf.erase(it);
            }
        }
    }
}

void readTaskHandler(int clientfd);
static void heartbeatTaskHandler(int clientfd);
string getCurrentTime();
void mainMenu(int);
void showCurrentUserData();

// ============================================================
// main：发送线程
// ============================================================
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3)
    {
        cerr << "command invalid! example: ./ChatClient 127.0.0.1 6000" << endl;
        exit(-1);
    }
```

登录后才周期发送心跳。发送下一轮前若上一轮仍未确认，只提示连接可能不稳定而不立即断开，减少短暂调度延迟造成误判；收到 HEARTBEAT_ACK 后接收线程会重新设置确认标志。 这里启动或维护心跳后台任务；它与消息重试线程独立，前者维护在线路由租约，后者只关心业务 ACK。

### 片段 19：第 495-516 行

```cpp
    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd) { cerr << "socket create error" << endl; exit(-1); }

    sockaddr_in server;
    memset(&server, 0, sizeof(sockaddr_in));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip);

    if (-1 == connect(clientfd, (sockaddr *)&server, sizeof(sockaddr_in)))
    { cerr << "connect server error" << endl; close(clientfd); exit(-1); }

    std::thread readTask(readTaskHandler, clientfd);
    readTask.detach();

    // 启动重试线程：后台扫描 pending_map，对超时未收到ACK的消息重发
    g_retryClientFd = clientfd;
    std::thread retryTask(retryTaskHandler);
    retryTask.detach();
```

进程按命令行地址创建 TCP socket，并把文本 IP 与端口转换为网络字节序后连接。连接成功后再启动接收、重试、顺序冲刷和心跳线程，避免后台任务在 fd 尚不可用时抢先发送。

### 片段 20：第 517-546 行

```cpp

    std::thread orderFlushTask(orderFlushTaskHandler);
    orderFlushTask.detach();

    // 启动心跳线程：定时探测连接可用性
    std::thread heartbeatTask(heartbeatTaskHandler, clientfd);
    heartbeatTask.detach();

    for (;;)
    {
        cout << "========================" << endl;
        cout << "1. login" << endl;
        cout << "2. register" << endl;
        cout << "3. quit" << endl;
        cout << "========================" << endl;
        cout << "choice:";
        int choice = 0;
        cin >> choice;
        cin.get();

        switch (choice)
        {
        case 1:
        {
            int id = 0; char pwd[50] = {0};
            cout << "userid:"; cin >> id; cin.get();
            cout << "userpassword:"; cin.getline(pwd, 50);
            json js;
            js["msgid"] = LOGIN_MSG;
            js["id"] = id;
```

登录后才周期发送心跳。发送下一轮前若上一轮仍未确认，只提示连接可能不稳定而不立即断开，减少短暂调度延迟造成误判；收到 HEARTBEAT_ACK 后接收线程会重新设置确认标志。 这里处理的不是心跳，而是序号洞超时：缓冲区等待超过阈值后，从当前最小可用序号继续输出，避免后续聊天永远被缺失序号阻塞。

### 片段 21：第 547-575 行

```cpp
            js["password"] = pwd;
            g_isLoginSuccess = false;
            const uint64_t version = responseVersion();
            // 使用帧协议发送
            if (sendFrame(clientfd, js.dump()) == -1)
            {
                cerr << "send login msg error" << endl;
                break;
            }
            waitForResponse(version);
            if (g_isLoginSuccess) { isMainMenuRunning = true; mainMenu(clientfd); }
        }
        break;
        case 2:
        {
            char name[50] = {0}, pwd[50] = {0};
            cout << "username:"; cin.getline(name, 50);
            cout << "userpassword:"; cin.getline(pwd, 50);
            json js;
            js["msgid"] = REG_MSG;
            js["name"] = name;
            js["password"] = pwd;
            const uint64_t version = responseVersion();
            // 使用帧协议发送
            if (sendFrame(clientfd, js.dump()) == -1)
            {
                cerr << "send reg msg error" << endl;
                break;
            }
```

主线程发送登录/注册后记录响应版本，并在条件变量上等待版本变化。接收线程处理响应后递增版本再唤醒；使用版本谓词可抵抗虚假唤醒，也不会把上一次响应误当成当前请求完成。 这一段把前台登录/注册请求与接收线程响应配对：发送前记录版本，等待被唤醒后再依据共享成功标志进入菜单或继续选择。

### 片段 22：第 576-605 行

```cpp
            waitForResponse(version);
        }
        break;
        case 3:
            close(clientfd); exit(0);
        default:
            cerr << "invalid input!" << endl; break;
        }
    }
    return 0;
}

// 处理注册的响应逻辑
void doRegResponse(json &responsejs)
{
    if (0 != responsejs["errno"].get<int>())
        cerr << "name is already exist, register error!" << endl;
    else
        cout << "name register success, userid is " << responsejs["id"]
             << ", do not forget it!" << endl;
}

// 处理登录的响应逻辑
void doLoginResponse(json &responsejs)
{
    if (0 != responsejs["errno"].get<int>())
    {
        cerr << responsejs["errmsg"] << endl;
        g_isLoginSuccess = false;
    }
```

主线程发送登录/注册后记录响应版本，并在条件变量上等待版本变化。接收线程处理响应后递增版本再唤醒；使用版本谓词可抵抗虚假唤醒，也不会把上一次响应误当成当前请求完成。 这一段把前台登录/注册请求与接收线程响应配对：发送前记录版本，等待被唤醒后再依据共享成功标志进入菜单或继续选择。 源码在这一段特别限定了“处理注册的响应逻辑”，因此解释范围止于该局部步骤。

### 片段 23：第 606-627 行

```cpp
    else
    {
        g_currentUser.setId(responsejs["id"].get<int>());
        g_currentUser.setName(responsejs["name"]);
        // 登录成功后设置全局用户ID，并作为雪花workerId的一部分
        g_myUserId = g_currentUser.getId();
        g_snowflake.setWorkerId(static_cast<uint16_t>(g_myUserId & 0x3FF));

        if (responsejs.contains("friends"))
        {
            g_currentUserFriendList.clear();
            vector<string> vec = responsejs["friends"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                User user;
                user.setId(js["id"].get<int>());
                user.setName(js["name"]);
                user.setState(js["state"]);
                g_currentUserFriendList.push_back(user);
            }
        }
```

登录响应中的好友、群和离线消息数组保存的是嵌套 JSON 字符串。客户端逐项再次 parse，构造本地 User/Group 列表；离线聊天消息不直接打印，而是送入同一顺序缓冲逻辑，保证在线与恢复消息采用一致展示规则。

### 片段 24：第 628-650 行

```cpp

        if (responsejs.contains("groups"))
        {
            g_currentUserGroupList.clear();
            vector<string> vec1 = responsejs["groups"];
            for (string &groupstr : vec1)
            {
                json grpjs = json::parse(groupstr);
                Group group;
                group.setId(grpjs["id"].get<int>());
                group.setName(grpjs["groupname"]);
                group.setDesc(grpjs["groupdesc"]);
                vector<string> vec2 = grpjs["users"];
                for (string &userstr : vec2)
                {
                    GroupUser user;
                    json js = json::parse(userstr);
                    user.setId(js["id"].get<int>());
                    user.setName(js["name"]);
                    user.setState(js["state"]);
                    user.setRole(js["role"]);
                    group.getUsers().push_back(user);
                }
```

循环遍历 `groups`、`users`，把每个元素独立转换、投递或校验。结果按遍历顺序追加，某个元素失败时由本片段的状态变量或断言记录，不能用一次总体成功掩盖单项失败。

### 片段 25：第 651-685 行

```cpp
                g_currentUserGroupList.push_back(group);
            }
        }

        showCurrentUserData();

        if (responsejs.contains("offlinemsg"))
        {
            vector<string> vec = responsejs["offlinemsg"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                printOrderedIncoming(js);
            }
        }
        g_isLoginSuccess = true;
    }
}

// ============================================================
// 1/2/3子线程 - 接收线程
// 使用帧协议接收，每次 recvFrame() 得到完整 JSON
// 处理 MSG_ACK：从 pending_map 移除对应消息
// ============================================================
void readTaskHandler(int clientfd)
{
    for (;;)
    {
        // 使用帧协议接收完整消息，无粘包/拆包问题
        string payload = recvFrame(clientfd);
        if (payload.empty())
        {
            close(clientfd);
            exit(-1);
        }
```

登录响应中的好友、群和离线消息数组保存的是嵌套 JSON 字符串。客户端逐项再次 parse，构造本地 User/Group 列表；离线聊天消息不直接打印，而是送入同一顺序缓冲逻辑，保证在线与恢复消息采用一致展示规则。 发送前创建 UUID、递增当前会话序号并立即插入本地消息；随后登记 pending 和发送 payload，用户能即时看到“发送中”，ACK 再更新为已发送。

### 片段 26：第 686-707 行

```cpp

        json js;
        try { js = json::parse(payload); }
        catch (...) { cerr << "[recv] JSON parse error" << endl; continue; }

        int msgtype = js["msgid"].get<int>();

        // 处理服务端ACK：从 pending_map 移除对应消息
        if (MSG_ACK == msgtype)
        {
            string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";
            int ackState = js.contains("ack_state") ? js["ack_state"].get<int>() : -1;
            if (ackState == ACK_OK)
            {
                removePending(msgId); // 服务端已确认，停止重试
            }
            else if (ackState == ACK_DEDUP)
            {
                // 重复消息被服务端去重，同样移出pending（不需要重试）
                removePending(msgId);
                cerr << "[ack] message " << msgId << " was deduped by server" << endl;
            }
```

`message_id` 贯穿发送、ACK、重试和接收去重：同一业务消息重发时 id 不变，服务端才能识别重复；`ack_state` 则告诉发送者是已接受、已去重还是处理失败，而不是仅凭 TCP 写成功判断业务成功。

### 片段 27：第 708-732 行

```cpp
            else
            {
                cerr << "[ack] message " << msgId << " failed on server, ack_state=" << ackState << endl;
            }
            continue;
        }

        if (ONE_CHAT_MSG == msgtype)
        {
            printOrderedIncoming(js);
            continue;
        }

        if (GROUP_CHAT_MSG == msgtype)
        {
            printOrderedIncoming(js);
            continue;
        }

        // 心跳ACK：标记连接可用
        if (HEARTBEAT_MSG_ACK == msgtype)
        {
            g_heartbeatAcked = true;
            continue;
        }
```

`HEARTBEAT_MSG_ACK`（13）不是聊天消息的 `MSG_ACK`（11）：它仅表示服务端已收到并通过身份校验这一轮心跳。CLI 收到它后把 `g_heartbeatAcked` 设为 true，供下一轮心跳前检查“上一轮是否有回应”；它不移除聊天消息的 pending 项，也不表示接收方已收到聊天内容。服务端当前在 Redis 条件续期不匹配时仍回 13，因此它也不能证明该连接仍持有 Redis 路由；返回中的 `echo_ts` 只可供客户端计算这一轮请求/应答耗时。Web 端当前收到 13 后不维护额外状态，直接忽略。

### 片段 28：第 733-754 行

```cpp

        if (ERROR_MSG == msgtype)
        {
            cerr << "[server] " << js.value("message", string("request rejected"))
                 << " (code=" << js.value("code", -1) << ")" << endl;
            continue;
        }

        if (LOGIN_MSG_ACK == msgtype)
        {
            doLoginResponse(js);
            notifyResponse();
            continue;
        }

        if (REG_MSG_ACK == msgtype)
        {
            doRegResponse(js);
            notifyResponse();
            continue;
        }
    }
```

主线程发送登录/注册后记录响应版本，并在条件变量上等待版本变化。接收线程处理响应后递增版本再唤醒；使用版本谓词可抵抗虚假唤醒，也不会把上一次响应误当成当前请求完成。 这一段把前台登录/注册请求与接收线程响应配对：发送前记录版本，等待被唤醒后再依据共享成功标志进入菜单或继续选择。 这项说明对应第 733-754 行的局部收尾，不代替前后片段的业务含义。

### 片段 29：第 755-786 行

```cpp
}

// 显示当前登录成功用户的基本信息
void showCurrentUserData()
{
    cout << "======================login user======================" << endl;
    cout << "current login user => id:" << g_currentUser.getId() << " name:" << g_currentUser.getName() << endl;
    cout << "----------------------friend list---------------------" << endl;
    for (User &user : g_currentUserFriendList)
        cout << user.getId() << " " << user.getName() << " " << user.getState() << endl;
    cout << "----------------------group list----------------------" << endl;
    for (Group &group : g_currentUserGroupList)
    {
        cout << group.getId() << " " << group.getName() << " " << group.getDesc() << endl;
        for (GroupUser &user : group.getUsers())
            cout << user.getId() << " " << user.getName() << " " << user.getState() << " " << user.getRole() << endl;
    }
    cout << "======================================================" << endl;
}

// 获取系统时间
string getCurrentTime()
{
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm localTime = {};
    localtime_r(&tt, &localTime);
    char date[60] = {0};
    snprintf(date, sizeof(date), "%d-%02d-%02d %02d:%02d:%02d",
             localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
             localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    return std::string(date);
}
```

SQL 的数值字段按十进制写入，外部字符串则应在前一阶段完成 MySQL 转义。查询列顺序与后面的 row 下标一一对应；一旦调整 SELECT 列表，也必须同步对象装配顺序，避免名称、状态或角色错位。

### 片段 30：第 787-813 行

```cpp

// 命令处理函数声明
void help(int fd = 0, string str = "");
void chat(int, string);
void addfriend(int, string);
void creategroup(int, string);
void addgroup(int, string);
void groupchat(int, string);
void loginout(int, string);

unordered_map<string, string> commandMap = {
    {"help", "显示所有支持的命令，格式help"},
    {"chat", "一对一聊天，格式chat:friendid:message"},
    {"addfriend", "添加好友，格式addfriend:friendid"},
    {"creategroup", "创建群组，格式creategroup:groupname:groupdesc"},
    {"addgroup", "加入群组，格式addgroup:groupid"},
    {"groupchat", "群聊，格式groupchat:groupid:message"},
    {"loginout", "注销，格式loginout"}};

unordered_map<string, function<void(int, string)>> commandHandlerMap = {
    {"help", help},
    {"chat", chat},
    {"addfriend", addfriend},
    {"creategroup", creategroup},
    {"addgroup", addgroup},
    {"groupchat", groupchat},
    {"loginout", loginout}};
```

菜单把冒号前的命令名映射到处理函数，冒号后的文本保留为参数。关系命令只带目标 id；聊天命令还生成 message_id、会话序号并在发送前登记 pending，确保 ACK 到得很快时也能正确移除待确认项。

### 片段 31：第 814-842 行

```cpp

void mainMenu(int clientfd)
{
    help();
    char buffer[1024] = {0};
    while (isMainMenuRunning)
    {
        cin.getline(buffer, 1024);
        string commandbuf(buffer);
        string command;
        int idx = commandbuf.find(":");
        if (-1 == idx)
            command = commandbuf;
        else
            command = commandbuf.substr(0, idx);
        auto it = commandHandlerMap.find(command);
        if (it == commandHandlerMap.end())
        { cerr << "invalid input command!" << endl; continue; }
        it->second(clientfd, commandbuf.substr(idx + 1, commandbuf.size() - idx));
    }
}

void help(int, string)
{
    cout << "show command list >>> " << endl;
    for (auto &p : commandMap)
        cout << p.first << " : " << p.second << endl;
    cout << endl;
}
```

菜单把冒号前的命令名映射到处理函数，冒号后的文本保留为参数。关系命令只带目标 id；聊天命令还生成 message_id、会话序号并在发送前登记 pending，确保 ACK 到得很快时也能正确移除待确认项。 这段继续落实“C++ 命令行客户端”的当前分支，并把已确认结果交给紧接着的状态更新；失败路径不会伪装成成功响应。

### 片段 32：第 843-872 行

```cpp

void addfriend(int clientfd, string str)
{
    int friendid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_FRIEND_MSG;
    js["id"] = g_currentUser.getId();
    js["friendid"] = friendid;
    // 帧协议发送
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send addfriend msg error" << endl;
}

void chat(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx) { cerr << "chat command invalid!" << endl; return; }
    int friendid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1, str.size() - idx);
    json js;
    js["msgid"] = ONE_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["toid"] = friendid;
    js["msg"] = message;
    js["time"] = getCurrentTime();
    // 单聊：按目标接收者维度递增序号（toid 独立）
    js["client_seq"] = nextClientSeqForPeer(friendid);
    // 携带唯一 message_id，加入 pending 等待ACK
    string msgId = generateMsgId();
```

菜单把冒号前的命令名映射到处理函数，冒号后的文本保留为参数。关系命令只带目标 id；聊天命令还生成 message_id、会话序号并在发送前登记 pending，确保 ACK 到得很快时也能正确移除待确认项。 好友命令把当前登录 id 与目标 id 组成请求，不创建 pending，因为关系操作当前协议没有消息 ACK/重试语义。

### 片段 33：第 873-897 行

```cpp
    js["message_id"] = msgId;
    string payload = js.dump();
    addPending(msgId, payload);
    // 帧协议发送
    if (sendFrame(clientfd, payload) == -1)
    {
        cerr << "send chat msg error" << endl;
        removePending(msgId); // 发送失败立即移除
    }
}

void creategroup(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx) { cerr << "creategroup command invalid!" << endl; return; }
    string groupname = str.substr(0, idx);
    string groupdesc = str.substr(idx + 1, str.size() - idx);
    json js;
    js["msgid"] = CREATE_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupname"] = groupname;
    js["groupdesc"] = groupdesc;
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send creategroup msg error" << endl;
}
```

这部分完成“C++ 命令行客户端”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 发送前创建 UUID、递增当前会话序号并立即插入本地消息；随后登记 pending 和发送 payload，用户能即时看到“发送中”，ACK 再更新为已发送。

### 片段 34：第 898-934 行

```cpp

void addgroup(int clientfd, string str)
{
    int groupid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupid"] = groupid;
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send addgroup msg error" << endl;
}

void groupchat(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx) { cerr << "groupchat command invalid!" << endl; return; }
    int groupid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1, str.size() - idx);
    json js;
    js["msgid"] = GROUP_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["groupid"] = groupid;
    js["msg"] = message;
    js["time"] = getCurrentTime();
    // 群聊：按群组维度递增序号（groupid 独立）
    js["client_seq"] = nextClientSeqForGroup(groupid);
    // 携带唯一 message_id
    string msgId = generateMsgId();
    js["message_id"] = msgId;
    string payload = js.dump();
    addPending(msgId, payload);
    if (sendFrame(clientfd, payload) == -1)
    {
        cerr << "send groupchat msg error" << endl;
        removePending(msgId);
    }
```

菜单把冒号前的命令名映射到处理函数，冒号后的文本保留为参数。关系命令只带目标 id；聊天命令还生成 message_id、会话序号并在发送前登记 pending，确保 ACK 到得很快时也能正确移除待确认项。 群聊以 groupId 作为会话序号维度，生成唯一 message_id 后先登记 pending 再发帧；重试沿用同一 payload，服务端才能幂等。

### 片段 35：第 935-949 行

```cpp
}

void loginout(int clientfd, string)
{
    json js;
    js["msgid"] = LOGINOUT_MSG;
    js["id"] = g_currentUser.getId();
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send loginout msg error" << endl;
    else
    {
        isMainMenuRunning = false;
        g_myUserId = 0; // 注销后停止心跳
    }
}
```

这部分完成“C++ 命令行客户端”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 登出只发送当前连接绑定的用户 id，随后由服务端条件释放节点路由；客户端不应自行伪造 offline 状态代替这次业务清理。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？
