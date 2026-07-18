# `frameprotocol.hpp` 讲解

## 作用概览

该头文件定义客户端与服务端共用的 TCP 应用层帧协议：4 字节网络序长度头加 JSON 负载。它只依赖标准库和 socket 字节序函数，因此核心协议可以独立单元测试。

## 按学习顺序讲解

### 常量

`FRAME_HEADER_LEN=4` 固定头长；`FRAME_MAX_PAYLOAD=4 MiB` 限制单帧大小，避免异常长度触发过度分配。

### `encodeFrame(payload)`

先拒绝空负载和超大负载，再用 `htonl` 把长度转为网络字节序，最后拼接长度头与原始字节。返回值可以直接交给 socket/Muduo 发送。

### `decodeFrameLength(header)`

使用 `memcpy` 读取可能未对齐的 4 字节，再用 `ntohl` 恢复主机序长度。避免直接强转指针带来的对齐和别名问题。

### `isValidPayloadLength(payloadLen)`

集中判断长度必须处于 `(0, 4 MiB]`，使客户端与服务端使用同一边界规则。

## 函数详细说明

### `encodeFrame(const std::string &payload)`

- **输入**：未经编码的业务负载，项目中通常是 `json.dump()` 的 UTF-8 字符串。函数不关心 JSON 内容，只关心字节长度。
- **返回**：新的二进制字符串，前 4 字节是 payload 长度，后面是原始 payload；调用方可以一次性交给 `send` 或 Muduo 的 `conn->send`。
- **执行过程**：先判断 payload 非空且不超过 4 MiB；再把 `size()` 转成 32 位整数并通过 `htonl` 转为大端；`reserve` 一次性预留总容量，随后追加帧头和正文，减少扩容复制。
- **异常与边界**：长度不合法时抛出 `std::length_error`。它只负责生成完整帧，不保证一次系统调用就能把全部字节发出，命令行客户端仍需要循环 `send`。
- **调用关系**：`FrameCodec::encode` 和命令行客户端 `sendFrame` 都复用它，从源头避免两端协议实现漂移。

### `decodeFrameLength(const char *header)`

- **输入**：至少指向 4 个可读字节的地址，调用者必须先确认缓冲区长度。
- **返回**：主机字节序的无符号 32 位 payload 长度。
- **执行过程**：先 `memcpy` 到局部变量，再用 `ntohl` 转换。没有直接写成 `*reinterpret_cast<uint32_t*>(header)`，是为了避免未对齐访问和严格别名问题。
- **边界**：该函数只解码，不判断 0 或超大长度；调用方必须继续调用 `isValidPayloadLength`。

### `isValidPayloadLength(std::uint32_t payloadLen)`

- **输入/返回**：输入解码后的长度；只有 1～4 MiB 返回 true。
- **作用**：把安全边界集中到公共协议层，网关、客户端、服务端不会各自使用不同上限。
- **边界含义**：拒绝 0 可避免解码循环在空帧上无进展；限制最大值可降低恶意长度头导致的内存与连接占用风险。


## 面试重点

重要性高。常见问题：为什么 TCP 需要应用层帧？TCP 是字节流，没有消息边界；为什么长度头用网络序？跨端字节序一致；如何处理粘包/半包？缓存不足一帧的数据，完整后再消费。
