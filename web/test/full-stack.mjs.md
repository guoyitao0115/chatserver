# `full-stack.mjs` 讲解

## 作用概览

这是快速双节点全链路测试，从原生 WebSocket 客户端穿过 Node 网关、C++ 服务、Redis、RabbitMQ 和 MySQL，覆盖主业务路径。

## 按学习顺序讲解

- `encodeClientFrame(value)`：生成 masked WebSocket 文本帧，供这个独立 smoke test 使用。
- `parseFrames(buffer,onMessage)`：增量拆服务端帧并解析 JSON。
- `createClient(port)`：完成 upgrade，内部 `deliver` 把消息交给匹配 waiter 或缓存；返回 `send/waitFor/close`。
- 顶层场景依次：在 8080/8081 注册并登录两个用户；跨节点发送并验证 ACK 和正文；伪造发送者应返回 401；心跳应答；接收者注销后消息应 ACK 并在重新登录时从离线列表恢复。
- `finally` 始终关闭所有客户端。

## 函数详细说明

### `encodeClientFrame(value)`

把业务对象序列化为 UTF-8 JSON，并构造 FIN + text opcode 的客户端 WebSocket 帧。正文长度小于 126 时使用短头，否则使用 16 位扩展长度；随机 4 字节 mask 被写入头部并与正文异或。这个 smoke 脚本只发送较小消息，所以未实现大于 65535 字节的分支，完整测试工具则覆盖三种长度。

### `parseFrames(buffer, onMessage)`

增量读取网关返回的服务端 WebSocket 帧。函数检查头部和完整 payload 是否到齐，解析 7/16/64 位长度，把正文 JSON 交给回调，并返回剩余半帧。循环允许同一个 TCP chunk 中包含多个响应。

该简化解析器默认所有收到的帧都是 JSON 文本，没有检查 opcode、FIN 和控制帧；它适合受控 smoke test，不应直接作为通用 WebSocket 实现。

### `createClient(port)`

函数用原生 TCP 连接指定网关端口，手工完成 Upgrade。握手前的字节存入 raw，找到响应头结束标记后断言 101，并把同包剩余字节转交 parseFrames。5 秒 deadline 和 socket error 让启动失败快速暴露。

内部 `deliver` 查找第一个谓词匹配的 waiter，否则缓存消息；返回对象的 `waitFor` 也先搜索缓存再登记观察者。每个 waiter 带 8 秒超时，并在 resolve 时清理 timer。`close` 直接销毁 socket，finally 中无论中途哪条断言失败都可以释放连接。

### API `send(value)`

调用 `encodeClientFrame` 后写 socket。这个快速脚本没有显式检查写入背压或连接关闭状态，失败通常由 socket error 或后续响应超时体现；公共 E2E 客户端提供了更完整的关闭保护。

### API `waitFor(predicate)`

先从已到达缓存中取出第一个匹配响应，否则创建带超时的 Promise waiter。场景通常用 `msgid + message_id` 匹配 ACK/消息，避免并发响应串线。谓词抛异常会影响 deliver，因此应只做简单字段判断。

### API `close()`

直接调用 `socket.destroy()`，用于 smoke test 的强制清理。它会触发网关后端连接关闭和服务端异常断线清理，但不像标准 close 帧那样验证优雅关闭协议。

### 顶层场景：用户准备

脚本为用户名增加时间和随机后缀，避免重复执行撞到历史数据。两个客户端分别连接 port1 和 port2，依次注册并检查 `errno`，再登录并确认成功。只有两个端口确实对应不同 C++ 节点时，后续消息才覆盖 Redis 路由和 RabbitMQ 跨节点路径。

### 顶层场景：跨节点消息与 ACK

client1 发送带固定 `message_id`、时间和 `client_seq` 的单聊。脚本同时验证发送方获得 `ACK_OK`、client2 收到相同 ID，并检查正文保真。ACK 证明服务端接受投递路径，收件断言证明消息真正穿过网关到达目标客户端。

### 顶层场景：身份伪造与心跳

脚本故意让 client1 在 JSON 中声明自己是 reg2，期望统一错误消息且 code 为 401。这验证服务端使用连接绑定身份，而不是相信请求字段。随后发送心跳并等待专用 ACK，验证登录会话和 Redis 路由续期仍正常。

### 顶层场景：离线持久化与重登

client2 主动注销、短暂等待清理并关闭，然后 client1 发送新 ID 消息。发送方 ACK_OK 表示离线落库成功；新建 client2 连接重新登录后，脚本扫描 `offlinemsg` 的嵌套 JSON，要求找到同一 ID。这个路径覆盖 MySQL 离线表，但快速脚本没有再登录第二次检查“不重复拉取”，该性质由 reliability 测试负责。

### `finally` 清理

不论注册、跨节点、权限或离线断言在哪一步失败，finally 都关闭两个原客户端和可选重连客户端。可靠清理防止残留在线路由影响下一次同账号测试，也让 Node 进程不会被 socket 句柄拖住。

## 面试重点

重要性高。常见问题：该测试如何证明跨节点而非本地投递？两个客户端连接到分别绑定不同 C++ 节点的网关；它和 reliability 测试区别？这里只做小而快的主链路 smoke，后者覆盖并发、顺序、去重及更多边界。
