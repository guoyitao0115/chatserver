#ifndef FRAMECODEC_HPP
#define FRAMECODEC_HPP

/*
 * ============================================================
 * 【改动1】应用层帧协议 —— 解决TCP粘包/拆包问题
 * ============================================================
 * 帧结构（网络字节序，大端）：
 *   [4字节 payload_len][payload(json字符串)]
 *
 * payload_len：后续payload的字节数（不含自身4字节）
 * payload    ：JSON字符串，与原有业务字段完全兼容
 *
 * 设计说明：
 *   - 采用固定4字节长度头，简单高效
 *   - body仍为JSON，对业务层零侵入
 *   - 支持任意大小消息，不再受1024字节缓冲区限制
 *   - muduo Buffer 天然支持按字节追加，可以安全地跨多次onMessage拼帧
 * ============================================================
 */

#include <muduo/net/TcpConnection.h>
#include <muduo/net/Buffer.h>
#include <muduo/base/Logging.h>
#include "frameprotocol.hpp"
#include <string>
#include <functional>
using namespace std;
using namespace muduo;
using namespace muduo::net;

using chatserver::protocol::FRAME_HEADER_LEN;
using chatserver::protocol::FRAME_MAX_PAYLOAD;

/*
 * FrameCodec：帧编解码器
 *
 * 使用方式：
 *   1. 在ChatServer中替换原来的 buffer->retrieveAllAsString() 逻辑
 *   2. 调用 decode(buffer, callback) 可能触发0次或多次callback（对应0条或多条完整消息）
 *   3. 发送时调用 encode(payload) 得到带帧头的完整字节串再 conn->send()
 */
class FrameCodec
{
public:
    /**
     * 完整帧回调：每成功解出一个 payload 调用一次。
     * conn 和 Timestamp 原样传给业务层；payload 是独立字符串，不再依赖 Buffer。
     * 回调在调用 decode() 的 Muduo I/O 线程内同步执行，处理器应避免长时间阻塞。
     */
    using MessageCallback = std::function<void(const TcpConnectionPtr &,
                                               const string &payload,
                                               Timestamp)>;

    // codec 持有回调副本，其生命周期必须覆盖所有 decode() 调用。
    // 传入空 std::function 会在完整帧到达时抛出 bad_function_call，构造方应保证有效。
    explicit FrameCodec(MessageCallback cb) : _messageCb(std::move(cb)) {}

    /*
     * 编码：将 payload 打包成 [4字节长度][payload] 帧。
     * 返回完整帧字节串，调用方可直接 conn->send()。空 payload 或超过 4 MiB 时
     * 底层 encodeFrame() 抛出 length_error，调用方应把它作为本地协议错误处理。
     * 此静态函数没有共享状态，可由多个线程并发调用。
     */
    static string encode(const string &payload)
    {
        return chatserver::protocol::encodeFrame(payload);
    }

    /*
     * 解码：从 Muduo Buffer 中尽量多地提取完整帧。
     *
     * - 一个 Buffer 中有多帧时按字节流顺序逐帧回调，解决粘包；
     * - 只有半个帧头或半个 body 时不消费数据，留待下一次 onMessage，解决拆包；
     * - 长度为 0 或超过上限时关闭连接，避免死循环、超大内存占用和慢速攻击。
     *
     * buf 必须非空且属于 conn 当前回调上下文。该对象本身不加锁，预期每条连接的
     * Buffer 只由所属 EventLoop 线程访问；不要让多个线程同时对同一 Buffer decode。
     * 关闭连接是异步行为，发现协议错误后必须立即退出本轮解析。
     */
    void decode(const TcpConnectionPtr &conn, Buffer *buf, Timestamp ts)
    {
        // 循环解帧：buffer中可能连续有多个完整帧（粘包场景）
        while (true)
        {
            // 检查是否已有完整的帧头（至少4字节）
            if (buf->readableBytes() < static_cast<size_t>(FRAME_HEADER_LEN))
            {
                // 数据不足4字节，等待更多数据到来（拆包场景）
                break;
            }

            // 读取帧头（peek，不消耗buffer数据）
            uint32_t payloadLen = chatserver::protocol::decodeFrameLength(buf->peek());

            // 空帧与超大帧都属于协议错误，立即关闭，避免死循环或内存攻击
            if (!chatserver::protocol::isValidPayloadLength(payloadLen))
            {
                LOG_ERROR << "FrameCodec: invalid payload length (" << payloadLen
                          << "), close connection";
                conn->shutdown();
                break;
            }

            // 检查payload是否已全部到达
            if (buf->readableBytes() < static_cast<size_t>(FRAME_HEADER_LEN + payloadLen))
            {
                // payload尚未完整，等待更多数据（拆包场景）
                break;
            }

            // 消费帧头4字节
            buf->retrieve(FRAME_HEADER_LEN);
            // 提取payload字节串
            string payload = buf->retrieveAsString(payloadLen);

            // 回调业务层处理完整消息
            _messageCb(conn, payload, ts);
        }
    }

private:
    // 完整帧到达后的业务入口；FrameCodec 只负责边界解析，不理解 JSON 字段。
    MessageCallback _messageCb;
};

#endif // FRAMECODEC_HPP
