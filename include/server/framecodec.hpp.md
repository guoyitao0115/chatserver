# `framecodec.hpp` 讲解

## 作用概览

`FrameCodec` 把通用长度帧协议接入 Muduo Buffer：发送时编码，接收时可从一次回调中拆出零条、一条或多条完整消息，并保留未完成半包。

## 按学习顺序讲解

### `FrameCodec(MessageCallback cb)`

保存“完整负载到达”回调。编解码器不知道 JSON 业务，保持协议层独立。

### `encode(payload)`

静态代理到公共 `encodeFrame`，保证所有 Muduo 发送使用相同帧格式和大小限制。

### `decode(conn, buf, ts)`

循环执行：先等待 4 字节头；读取并校验长度；若整帧尚未到达则不消费；完整时消费头和 payload，再触发回调。循环使一次 TCP 读事件中的粘连多帧都能被处理。非法长度立即关闭连接，避免空帧死循环和超大帧攻击。

## 函数详细说明

### `FrameCodec(MessageCallback cb)`

- **参数**：接收一个完整消息回调，回调参数包含连接、完整 payload 和本次读事件时间戳。
- **状态变化**：通过 `std::move` 保存回调到 `_messageCb`，编解码器本身不保存每条连接的半包；未消费字节由每个连接自己的 Muduo `Buffer` 保存。
- **设计意义**：codec 只识别二进制帧，不解析 JSON，也不知道登录或聊天业务，从而保持网络协议层与业务层解耦。

### `static string encode(const string &payload)`

- **参数/返回**：输入业务字符串，返回带长度头的整帧。
- **执行过程**：直接委托 `chatserver::protocol::encodeFrame`，异常语义和 4 MiB 上限也完全复用。
- **调用位置**：服务端所有响应、ACK、聊天投递和错误响应都经此函数发送，避免某条路径遗漏帧头。

### `decode(const TcpConnectionPtr &conn, Buffer *buf, Timestamp ts)`

- **参数**：`conn` 用于回调和协议错误断连；`buf` 是该连接累计收到的字节；`ts` 传给业务层。
- **循环步骤**：
  1. 可读字节不足 4 时立即退出，数据留在 Buffer 等下一次读事件。
  2. 用 `peek()` 查看但不消费长度头；长度非法则 `shutdown` 并结束。
  3. 总字节不足 `4 + payloadLen` 时退出，保证不会提前消费半包。
  4. 完整时先 `retrieve(4)`，再精确取 payload，调用一次 `_messageCb`。
  5. 继续循环，因此一次读事件中粘连的多帧都会依次交付。
- **返回/副作用**：无返回值；副作用是消费 Buffer 中的完整帧、调用业务回调，或在协议错误时关闭连接。
- **并发边界**：Muduo 通常保证同一连接的消息回调在所属 I/O loop 串行执行；codec 无共享可变解析状态，因此可被服务器对象复用。


## 面试重点

重要性最高。常见问题：`peek` 和 `retrieve` 的区别、为什么不使用固定 1024 字节缓冲、半包为何必须留在 Buffer、回调可能被调用几次。可画出 `[len][json][len][json]` 的粘包解析过程。
