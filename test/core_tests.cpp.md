# `core_tests.cpp` 讲解

## 作用概览

**核心单元测试。** 不启动外部服务，直接验证帧长度边界、本地去重的 TTL/并发行为以及环境配置解析。这些测试失败通常表示基础组件契约被破坏。

阅读位置：`test/core_tests.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-28 行

```cpp
#include "config.hpp"
#include "frameprotocol.hpp"
#include "msgdedup.hpp"

#include <cstdlib>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void expect(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

static void testFrameProtocol()
{
    const std::string payload = R"({"msgid":1,"name":"测试"})";
    const std::string frame = chatserver::protocol::encodeFrame(payload);
    expect(frame.size() == chatserver::protocol::FRAME_HEADER_LEN + payload.size(), "frame size mismatch");
    expect(chatserver::protocol::decodeFrameLength(frame.data()) == payload.size(), "frame length decode mismatch");
    expect(frame.substr(chatserver::protocol::FRAME_HEADER_LEN) == payload, "frame payload mismatch");
    expect(!chatserver::protocol::isValidPayloadLength(0), "empty payload must be invalid");
    expect(!chatserver::protocol::isValidPayloadLength(chatserver::protocol::FRAME_MAX_PAYLOAD + 1), "oversized payload must be invalid");
```

帧测试先编码含中文的 JSON，检查总长度、解出的正文长度和原 payload 完全一致；随后分别验证长度 0、超过 4 MB 和空字符串编码会被拒绝。中文让测试同时覆盖“字节长度不能按字符数计算”这一边界。

读取先确认长度头完整，再判断正文是否已经收齐；不足时保留 Buffer 原状等待下一批字节。只有长度合法且正文完整才前移读指针，这同时处理了半包和一次到达多帧的粘包情况。

### 片段 2：第 29-56 行

```cpp

    bool emptyRejected = false;
    try
    {
        chatserver::protocol::encodeFrame("");
    }
    catch (const std::length_error &)
    {
        emptyRejected = true;
    }
    expect(emptyRejected, "empty payload was not rejected");
}

static void testDedup()
{
    MsgDedup dedup(2, 120);
    expect(!dedup.isDuplicate("a"), "first a must be new");
    expect(dedup.isDuplicate("a"), "second a must be duplicate");
    expect(!dedup.isDuplicate("b"), "first b must be new");
    expect(!dedup.isDuplicate("c"), "first c must be new");
    expect(dedup.isDuplicate("b"), "b must still be cached");
    // Inserting c evicted a, which was then the least-recently-used entry.
    expect(!dedup.isDuplicate("a"), "evicted a must be new");
    dedup.forget("a");
    expect(!dedup.isDuplicate("a"), "forgotten a must be accepted again");
    expect(!dedup.isDuplicate(""), "empty id must not deduplicate");
    expect(!dedup.isDuplicate(""), "empty id must remain non-deduplicated");
}
```

容量设为 2 后依次访问 a、b、c：第二次 a 必须重复，插入 c 时最久未使用的 a 被淘汰，而刚访问过的 b 仍在；forget(a) 后同 id 再次被接受，空 id 连续两次都不占缓存。

### 片段 3：第 57-80 行

```cpp

static void testConcurrentDedup()
{
    MsgDedup dedup(128, 120);
    std::atomic<int> firstSeen{0};
    std::vector<std::thread> workers;
    workers.reserve(32);

    for (int i = 0; i < 32; ++i)
    {
        workers.emplace_back([&dedup, &firstSeen]() {
            if (!dedup.isDuplicate("same-message-id"))
            {
                ++firstSeen;
            }
        });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }

    expect(firstSeen == 1, "concurrent duplicate check must accept exactly one message");
}
```

32 个线程共享同一个 MsgDedup，并同时检查完全相同的 id。每个线程只在返回“首次出现”时递增原子计数，最终必须恰好为 1，直接验证查找与登记处于同一临界区。

并发操作在等待器或收集器就绪后同时启动，避免响应太快而被测试代码错过。这里关注的是共享状态竞争：例如重复登录只能有一个赢家，或多连接突发发送后每组仍必须收齐自己的消息。

### 片段 4：第 81-103 行

```cpp

static void testConfig()
{
    unsetenv("CHAT_TEST_VALUE");
    expect(chatserver::config::envOr("CHAT_TEST_VALUE", "fallback") == "fallback", "environment fallback failed");
    setenv("CHAT_TEST_VALUE", "configured", 1);
    expect(chatserver::config::envOr("CHAT_TEST_VALUE", "fallback") == "configured", "environment override failed");

    setenv("CHAT_TEST_PORT", "7000", 1);
    expect(chatserver::config::envIntOr("CHAT_TEST_PORT", 6000) == 7000, "integer environment override failed");
    setenv("CHAT_TEST_PORT", "invalid", 1);
    expect(chatserver::config::envIntOr("CHAT_TEST_PORT", 6000) == 6000, "invalid integer fallback failed");
}

int main()
{
    testFrameProtocol();
    testDedup();
    testConcurrentDedup();
    testConfig();
    std::cout << "chat_core_tests: all assertions passed" << std::endl;
    return 0;
}
```

配置测试依次覆盖未设置字符串、字符串覆盖、合法整数和非法整数四种状态。非法 `7000` 之外的文本必须回到默认 6000，证明解析失败不会把端口静默变成 0。

## 面试重点

- 测试准备了什么外部状态或模拟组件，实际动作经过哪些模块？

- 每个断言证明的是返回值正确，还是“不丢、不重、不乱序、不可冒用”等系统性质？

- 如何避免测试自身的等待竞态和上轮残留状态造成假失败？
