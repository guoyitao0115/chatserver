# `gateway.test.mjs` 讲解

## 作用概览

这是 Node 内置 test runner 下的网关集成测试，用一个假的 C++ TCP 后端验证 HTTP、WebSocket 握手和双向帧转换，不依赖真实数据库或消息中间件。

## 按学习顺序讲解

- `tcpFrame(value)`：生成 C++ 协议长度帧。
- `maskedWebSocketFrame(value)`：生成浏览器方向的 masked 文本帧。
- `parseServerFrames(buffer,onMessage)`：解析网关返回的服务端 WebSocket 帧。
- `startMockBackend(received)`：随机端口启动 TCP 后端，增量解析网关的长度帧、记录业务 JSON，并回写模拟登录响应。
- `connectWebSocket(port)`：建立原生 TCP 并发送 WebSocket upgrade。
- 测试体 `HTTP health and WebSocket/TCP full bridge flow`：用 teardown 注册网关和 mock 清理；验证 `/health`、静态首页、404、协议内部函数、upgrade、浏览器→TCP、TCP→浏览器，以及 ping/pong/关闭等桥接行为。内部 `deliver(text)` 收集并匹配响应 waiter。

## 函数详细说明

### `tcpFrame(value)`

函数把 mock 后端要发送的对象序列化为 JSON Buffer，分配 4 字节大端长度头和正文，返回与 C++ 服务完全相同的 TCP 帧。网关若正确转换，浏览器测试客户端最终应收到对应 WebSocket 文本消息。

### `maskedWebSocketFrame(value)`

把对象或原始字符串编码成浏览器到网关方向的 masked 文本帧。短正文使用 7 位长度，较长正文使用 16 位扩展长度，随机 mask 写入头部并逐字节异或。测试故意不调用浏览器 WebSocket API，以便在 Node 内置测试中精确控制线上的字节。

### `parseServerFrames(buffer, onMessage)`

从网关返回的服务端帧中解析 7/16/64 位长度，正文到齐后转成 UTF-8 文本交给回调；半帧尾部返回给下次 data。服务端帧按规范不需要 mask。该测试解析器不处理分片和控制帧，只覆盖网关业务响应路径。

### `startMockBackend(received)`

函数在 `127.0.0.1` 的随机端口启动真实 `net.Server`。每条连接维护自己的 TCP 累积 Buffer，循环读取 4 字节长度和完整 JSON；收到对象后推入外部 `received` 数组，让测试断言浏览器请求确实到达后端。

mock 按消息类型返回确定响应：登录回登录 ACK、单聊回消息 ACK、心跳回 echo ACK。它只模拟传输契约，不连接 MySQL/Redis/RabbitMQ，因此网关错误能与后端业务错误隔离。listen 被 Promise 化，返回时端口已经可用。

### `connectWebSocket(port)`

函数建立原生 TCP，生成随机 `Sec-WebSocket-Key` 并发送 Upgrade 请求。data 回调在握手阶段累积头部，断言 101 后把剩余字节交给帧解析；升级完成后直接拼接 frameBuffer。2 秒超时用于发现网关没有完成握手。

返回 API 封装 `send`、`nextMessage` 和 `close`。它是该测试文件的受控客户端，不依赖第三方 WebSocket 包，从而能直接验证网关是否接受标准 masked 帧。

### 内部 `deliver(text)`

把帧正文 JSON.parse。如果存在等待下一条消息的 waiter，就取队首并 resolve；否则放进 messages 缓存。这里按到达顺序匹配而非谓词匹配，因为测试严格串行地“发送一次—等待一次”，逻辑更简单。

### API `nextMessage()`

缓存非空时立即 shift 最早消息；否则返回带 2 秒超时的 Promise，并把 resolver 放入队列。resolver 包装会先清 timer，避免成功后超时回调再次触发。若把测试改成并发发送，应改用谓词匹配，防止响应顺序变化导致断言串线。

### 测试体 `HTTP health and WebSocket/TCP full bridge flow`

用例依次启动 mock、随机端口网关和原生客户端，并第一时间通过 `t.after` 注册清理。它先 fetch `/health`，确认 HTTP 服务与 JSON 状态；随后发送登录、单聊和心跳，既断言浏览器收到 mock 响应，也检查 `received` 中后端收到的原请求字段，覆盖双向转换。

最后发送缺少 `msgid` 的原始 JSON，要求网关返回 400 且 mock 收包数量不增加。这一负断言证明最小业务门禁在网关生效。teardown 关闭客户端、活动网关 socket 和 mock server，任一中间断言失败也不会遗留句柄。

## 面试重点

重要性高。可能问题：为什么使用 mock 后端？隔离网关问题并让测试快速稳定；集成测试和单元测试界限？它使用真实 socket/HTTP，但外部后端是可控替身。
