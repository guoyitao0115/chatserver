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
