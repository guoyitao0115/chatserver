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

constexpr std::size_t FRAME_HEADER_LEN = 4;
constexpr std::uint32_t FRAME_MAX_PAYLOAD = 4U * 1024U * 1024U;

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

inline std::uint32_t decodeFrameLength(const char *header)
{
    std::uint32_t netLen = 0;
    std::memcpy(&netLen, header, FRAME_HEADER_LEN);
    return ntohl(netLen);
}

inline bool isValidPayloadLength(std::uint32_t payloadLen)
{
    return payloadLen > 0 && payloadLen <= FRAME_MAX_PAYLOAD;
}

} // namespace protocol
} // namespace chatserver

#endif
