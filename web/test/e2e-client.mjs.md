# `e2e-client.mjs` 讲解

## 作用概览

这是所有 Web 端到端、可靠性和压力测试共用的原生 WebSocket 测试客户端。它不依赖浏览器或第三方库，直接用 TCP 完成握手与帧协议，因此能精确控制消息、超时和重复检测。

## 按学习顺序讲解

- `encodeClientFrame(value,opcode)`：序列化文本，生成随机 4 字节 mask，支持三种长度并按 RFC 要求对客户端 payload 掩码；也可生成 close 帧。
- `parseFrames(buffer,onMessage)`：增量解析服务端无 mask 帧，只处理文本 opcode，并给消息附加不可枚举接收时间用于延迟统计。
- `createClient(port,options)`：建立 TCP，发送 HTTP Upgrade，缓存握手和后续帧。内部 `removeWaiter` 删除超时观察者，`deliver` 优先满足谓词 waiter，否则进入消息队列。
- 返回 API 的 `send`/ `sendRawText`：发送对象或故意损坏的原始文本。
- `waitFor(predicate,timeout)`：先查已缓存消息，再注册带超时的 waiter。
- `collect(predicate,count,timeout)`：在总截止时间内收集指定数量。
- `drain(predicate)`：同步取走已缓存的匹配消息，用于检查延迟重复。
- `expectNoMessage(predicate,duration)`：在窗口内若出现匹配消息就失败，是去重测试的关键负断言。
- `close()`：发送标准 close 帧并结束 socket，使网关及时关闭后端连接。
- `register(...)`、`login(...)`：封装常用请求等待。
- `parseEmbedded(values)`：解析登录响应中的嵌套 JSON 字符串。
- `delay(milliseconds)`：Promise 版计时器。

## 函数详细说明

### `encodeClientFrame(value, opcode = 0x1)`

函数把对象 JSON 序列化，或直接接受原始字符串，再转为 UTF-8 Buffer。它根据正文长度选择 WebSocket 的 7 位、16 位或 64 位长度格式，设置 FIN 和 opcode，并生成 4 字节随机 mask。正文每个字节与循环 mask 异或后和头部拼接。

客户端到服务端的帧必须带 mask，因此测试客户端若省略这一步，合规网关应拒绝。opcode 参数还允许 `close()` 生成关闭帧；测试正文通常使用文本 opcode `0x1`。

### `parseFrames(buffer, onMessage)`

解析器从 offset 0 开始循环，只在基础头、扩展长度和完整正文都已到达时消费一帧；不足部分原样返回，等待下一次 socket data 拼接。服务端帧不带 mask，因此这里直接读取正文。文本帧被 JSON.parse 后交给回调，其他 opcode 被跳过。

函数给消息添加不可枚举 `__receivedAt`。它可用于端到端延迟计算，但 `JSON.stringify`、深比较和业务字段遍历不会把这个测试元数据当成协议内容。

### `createClient(port, options = {})`

该异步工厂选择 host 和默认超时，建立原生 TCP 连接，关闭 Nagle 延迟，并手工发送 HTTP Upgrade 请求。它分开保存握手缓冲和 WebSocket 帧缓冲：先查找 `\r\n\r\n` 并断言状态行为 101，握手后剩余的字节立即作为第一批帧解析，避免响应头和首帧同包时丢数据。

工厂等待 upgraded 标志后才返回 API，调用方因此不会在握手完成前发送业务帧。握手设有截止时间和 error 监听，网关不可达时测试会明确失败，而不是永久挂起。

### 内部 `removeWaiter(waiter)`

从等待者数组中定位并删除指定对象，主要在超时和负断言结束时使用。若不删除，过期谓词以后可能截获一条本应交给新请求的消息，还会造成数组持续增长。

### 内部 `deliver(message)`

每条解析后的消息先在 waiters 中寻找第一个谓词匹配者。找到后删除 waiter、清除其定时器并 resolve；没有观察者时把消息存入缓存队列。这个“观察者优先、否则缓存”的模型同时处理响应早到和等待先注册两种时序。

不同并发请求的谓词必须足够精确，通常同时匹配 `msgid` 和 `message_id`。只匹配 ACK 类型可能让一个请求拿走另一个请求的 ACK。

### socket `data` 与 `close` 回调

data 回调负责增量握手和帧解析，所有完整消息最终进入 deliver。close 回调设置关闭标志，取出全部未完成 waiter、清 timer 并 reject，确保等待中的测试立即知道连接断开，而不是等到各自超时。

### API `send(value)` 与 `sendRawText(value)`

`send` 在连接关闭时主动抛错，否则编码并写入 TCP；`sendRawText` 要求参数必须是字符串，再复用 send。后者用于发送语法损坏或字段不合法的 JSON 文本，验证网关和后端的错误处理，而普通对象路径会自动序列化为合法 JSON。

### API `waitFor(predicate, timeoutMs)`

函数先同步搜索已缓存消息，找到就移除并立即返回 Promise；否则创建 waiter，设置谓词、resolve/reject 与超时器。超时时先调用 removeWaiter，再抛出包含端口和等待时长的错误，便于判断是哪条链路未响应。

“先查缓存再注册”防止响应比测试代码调用 `waitFor` 更快；谓词机制让多个消息类型和多个并发 ID 可以共享同一连接。

### API `collect(predicate, count, timeoutMs)`

该函数在统一总截止时间内反复 `waitFor`，直到收集到指定数量。每轮传入的是剩余时间，而不是重新获得完整 timeout，所以少一条消息时用例会在预期总时限失败，不会因逐条等待被放大为 `count × timeout`。

### API `drain(predicate)`

函数从后向前遍历缓存，移除全部匹配消息，再用 unshift 保持原到达顺序返回。倒序删除可避免数组 splice 后下标前移导致漏检。它适合在主要收集结束后检查是否已有额外重复消息积压。

### API `expectNoMessage(predicate, durationMs)`

负断言先检查缓存中是否已经存在匹配项，有则立即失败；否则注册一个特殊 waiter，在观察窗口内一旦匹配消息到达就 reject，计时结束仍未收到才 resolve。只检查当前队列无法证明之后不会迟到，因此必须保留一个有界观察窗口。

该函数证明的是“给定 duration 内未观察到”，窗口外的极晚重复仍可能发生。测试应按重试/网络最大预期延迟设置合理时长。

### API `close()`

连接仍存活时编码标准 opcode `0x8` close 帧，并通过 `socket.end` 发送后结束写侧，让网关及时关闭对应后端 TCP。相比直接 destroy，它更接近浏览器正常关闭行为，也减少服务端清理尚未触发就开始下一步测试的竞态。

### `register(client, name, password, timeoutMs)`

封装注册请求和响应等待，谓词只接受 `REGISTER_ACK`。收到后断言 `errno === 0`，失败信息包含用户名与服务端错误，成功返回分配的用户 ID。场景脚本因此可以聚焦业务性质，不必重复样板断言。

### `login(client, id, password, timeoutMs)`

发送登录请求并等待 `LOGIN_ACK`，但不直接断言成功，因为可靠性测试需要观察重复登录失败等合法负路径。调用者根据场景检查 `errno`，并解析离线消息或好友群组字段。

### `parseEmbedded(values = [])`

逐项把服务端返回的 JSON 字符串解析成对象，已经是对象的值原样保留。它封装旧接口的双重 JSON 编码，便于离线消息、好友和群组断言访问真实字段。

### `delay(milliseconds)`

用 `setTimeout` 包装 Promise，供测试明确等待断线清理、故障注入窗口或负观察期。固定 delay 容易受慢机器影响，应只用于没有可观察事件可等待的场合；能等待 ACK、健康状态或进程输出时优先事件驱动条件。

## 面试重点

重要性高。可能问题：为什么测试客户端要实现 waiter 队列、如何证明“没有重复消息”、为什么延迟时间属性设为不可枚举、WebSocket 客户端为何必须 mask。该工具是压力与可靠性结论可信度的基础。
