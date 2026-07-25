# `frameprotocol.hpp` 讲解

## 作用概览

**公共长度帧协议。** 提供不依赖 Muduo 的 4 字节大端长度头编码、解码和阻塞式收发工具，主要供命令行客户端和核心测试复用。

阅读位置：`include/frameprotocol.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-37 行

```cpp
#ifndef FRAMEPROTOCOL_HPP
#define FRAMEPROTOCOL_HPP

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace chatserver
{
namespace protocol
{

// 线上的帧头固定占 4 字节，保存无符号 32 位 payload 长度（网络字节序）。
// 固定长度头使接收端无需扫描分隔符，也能在 TCP 字节流中准确识别消息边界。
constexpr std::size_t FRAME_HEADER_LEN = 4;

// 单帧业务负载上限为 4 MiB。该限制是协议的一部分：客户端和服务端必须一致，
// 用于阻止伪造长度头导致接收端无限等待或分配过大内存。
constexpr std::uint32_t FRAME_MAX_PAYLOAD = 4U * 1024U * 1024U;

/**
 * 将一段完整业务负载编码为可直接写入 TCP 的长度前缀帧。
 *
 * @param payload 通常是序列化后的 JSON；必须非空且不超过 FRAME_MAX_PAYLOAD。
 * @return 由“4 字节网络序长度头 + 原始 payload 字节”组成的新字符串。
 * @throws std::length_error 当 payload 为空或超过协议上限时抛出；调用方不应发送
 *         该消息。函数不执行网络 I/O，也不会修改传入字符串。
 *
 * 先 reserve 再 append，可避免构造大消息时发生多次扩容。长度转换为 uint32_t 前
 * 已经过上限校验，因此不会发生 size_t 截断。
 */
inline std::string encodeFrame(const std::string &payload)
{
    if (payload.empty() || payload.size() > FRAME_MAX_PAYLOAD)
    {
```

这些头文件把“公共长度帧协议”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 38-63 行

```cpp
        throw std::length_error("chat frame payload size is invalid");
    }

    const std::uint32_t netLen = htonl(static_cast<std::uint32_t>(payload.size()));
    std::string frame;
    frame.reserve(FRAME_HEADER_LEN + payload.size());
    frame.append(reinterpret_cast<const char *>(&netLen), FRAME_HEADER_LEN);
    frame.append(payload);
    return frame;
}

/**
 * 从 4 字节帧头读取 payload 长度并转换为主机字节序。
 *
 * @param header 至少指向 FRAME_HEADER_LEN 个可读字节；其有效性和生命周期由调用方
 *        保证，函数不会检查空指针或缓冲区长度。
 * @return 帧头声明的负载字节数。该结果仍需交给 isValidPayloadLength() 校验。
 *
 * 使用 memcpy 而非直接将 char* 强转为 uint32_t*，避免未对齐访问和严格别名问题。
 */
inline std::uint32_t decodeFrameLength(const char *header)
{
    std::uint32_t netLen = 0;
    std::memcpy(&netLen, header, FRAME_HEADER_LEN);
    return ntohl(netLen);
}
```

写出数据前补上 4 字节大端长度头，接收方因此能从连续 TCP 字节中判断一条 JSON 到哪里结束。例如两条消息合并到一次读取时，会按各自长度连续拆出，而不会把两个 JSON 拼成坏数据。

读取先确认长度头完整，再判断正文是否已经收齐；不足时保留 Buffer 原状等待下一批字节。只有长度合法且正文完整才前移读指针，这同时处理了半包和一次到达多帧的粘包情况。

### 片段 3：第 64-77 行

```cpp

/**
 * 判断长度是否属于当前协议允许的业务负载范围。
 * 空帧被视为协议错误；超过上限的帧应由连接层立即拒绝，不能继续等待其 body。
 */
inline bool isValidPayloadLength(std::uint32_t payloadLen)
{
    return payloadLen > 0 && payloadLen <= FRAME_MAX_PAYLOAD;
}

} // namespace protocol
} // namespace chatserver

#endif
```

这一接口片段规定“公共长度帧协议”对外可用的操作和对象必须长期保存的状态。调用者只依赖这里的契约；锁、SQL、网络错误和资源释放留在实现内部，因此更换基础设施不会迫使业务处理器改写所有调用点。

## 面试重点

- TCP 为什么必须自行处理半包和粘包，4 字节长度头如何完成增量解码？

- WebSocket 帧与后端 TCP 长度帧的边界分别在哪里，网关为什么不能承担最终鉴权？

- 最大帧长、半包超时和空闲超时各自防什么问题？
