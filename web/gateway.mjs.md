# `gateway.mjs` 讲解

## 作用概览

这是浏览器与原 C++ 服务之间的最小适配层。浏览器说 WebSocket，后端说“4 字节大端长度 + JSON”；网关只转换传输帧，不改业务字段，因此对既有 C++ 架构影响很小。同时它提供静态文件和健康检查。

## 按学习顺序讲解

### 帧函数

- `encodeTcpFrame(payload)`：把 UTF-8 文本变成与 C++ 一致的 4 字节长度帧，并执行 4 MiB 限制。
- `encodeWebSocketFrame(payload,opcode)`：生成服务端无 mask 的 WebSocket 帧，支持 7/16/64 位长度，默认文本 opcode。
- `parseWebSocketFrames(buffer,onFrame)`：增量解析浏览器发来的 masked 帧；处理三种长度和 mask 解码，拒绝分片、未 mask 及超大帧，返回未完成尾部供下次拼接。
- `isValidBusinessPayload(text)`：确保文本是 JSON 对象且包含整数 `msgid`，错误不进入 C++ 后端。

### `createGateway(options)`

读取端口、后端地址和静态目录，并创建 HTTP 服务。HTTP 回调处理：
- `/health` 返回网关状态和后端地址；
- favicon 返回 204；
- 静态白名单只允许 index/app/styles，避免任意路径读取；
- 文件响应设置 MIME、禁止缓存和 `nosniff`。

WebSocket upgrade 回调校验 `/ws` 和 key，然后建立一条专属后端 TCP 连接。内部 `closeBoth()` 是幂等清理函数，保证任一侧关闭都释放另一侧。后端数据按 TCP 长度帧增量拆分后编码为 WebSocket；浏览器数据处理 close、ping/pong、文本业务帧，再编码给 TCP。后端握手失败返回 502，升级后失败返回业务错误帧。

返回对象的：
- `listen()`：Promise 化监听，返回实际地址，方便测试使用随机端口。
- `close()`：销毁全部活动 socket，再关闭 HTTP 服务器，保证测试不遗留句柄。

文件末尾仅在直接执行时启动；作为测试模块 import 时不会监听。导出的 `protocolInternals` 便于对白盒协议函数测试。

## 函数详细说明

### `encodeTcpFrame(payload)`

函数把字符串按 UTF-8 编码成 `Buffer`，先检查字节数而不是 JavaScript 字符数是否落在允许范围，再分配“4 字节 + 正文长度”的连续缓冲。头部用大端无符号整数写入正文长度，随后复制正文。这与 C++ `FrameCodec` 的网络字节序约定完全一致。

检查字节数很重要：中文字符通常占多个 UTF-8 字节，只看 `string.length` 会低估网络载荷。超出 4 MiB 时直接抛错，避免网关替攻击者构造后端拒绝的大帧。

### `encodeWebSocketFrame(payload, opcode = 0x1)`

这是服务端到浏览器方向的 WebSocket 编码器。它设置 FIN 位和 opcode，根据 payload 长度选择 7 位、16 位或 64 位扩展长度，再拼接未掩码正文。服务端帧按协议不能像客户端帧那样强制 mask，因此这里只写长度而不生成掩码。

默认 opcode `0x1` 表示文本；同一函数也可发送 pong 或 close 控制帧。JavaScript 数字对超大 64 位长度存在精度限制，但项目的 4 MiB 上限远低于该边界。

### `parseWebSocketFrames(buffer, onFrame)`

函数以循环方式增量解析浏览器发来的 WebSocket 字节。每轮先确认基础 2 字节头存在，读取 FIN、opcode、mask 和基础长度；长度标记为 126/127 时继续等待并读取扩展长度。随后要求客户端帧带 4 字节 mask，并在正文到齐后逐字节异或还原数据。

一个 TCP 数据块可能含多帧，循环会连续调用 `onFrame(opcode, payload)`；最后返回尚未完整的尾部，调用方与下一次 `data` 拼接。函数拒绝未 mask、分片帧和超过上限的 payload，这些协议错误由上层关闭连接。控制帧还应遵守不分片、长度不超过 125 等规则，当前实现只覆盖项目所需子集。

### `isValidBusinessPayload(text)`

函数尝试 `JSON.parse`，要求结果是非数组对象，并且 `msgid` 为整数。验证失败返回 false，使明显损坏或不属于本项目协议的数据不进入 C++ 服务。这里只做最小协议门禁，具体字段、登录身份和权限仍由后端检查，不能把网关校验当作安全边界。

### `createGateway(options = {})`

这是网关工厂函数。它从 options、环境变量和默认值中解析 HTTP 监听端口、C++ 后端地址与静态目录，创建活动 socket 集合和 HTTP server。返回实例而不是 import 时直接监听，使测试可以注入随机端口和 mock 后端，并在用例结束后彻底关闭。

HTTP 请求回调先区分健康检查、favicon 和静态资源。静态文件通过固定 Map 白名单映射，而不是直接把 URL 拼到磁盘路径，避免 `../` 路径穿越；响应附带正确 MIME、`nosniff` 和禁用缓存。读取失败返回受控错误，不暴露本地文件路径。

### HTTP `upgrade` 回调

升级回调验证请求路径、`Upgrade` 语义和 `Sec-WebSocket-Key`，计算标准 SHA-1 accept 值并完成 101 握手。每个浏览器 WebSocket 对应创建一条独立后端 TCP 连接，这让原 C++ 服务仍把它视为普通聊天客户端，无需修改业务架构。

浏览器侧数据先由 `parseWebSocketFrames` 拆分：close 触发双向关闭，ping 立即回复 pong，文本帧经过 JSON 门禁后编码为 TCP 长度帧。后端侧则维护独立累积 Buffer，循环读取 4 字节大端长度和完整正文，再编码成服务端 WebSocket 文本帧。两侧分别处理自己的半包，不能共用同一种边界判断。

### `closeBoth()`

这是每个升级连接内部的幂等清理闭包。无论浏览器关闭、后端断开、握手失败还是解析异常，都只执行一次，并销毁/结束两边 socket、从活动集合删除句柄。幂等标志可避免两个 close/error 事件互相触发后重复操作已销毁资源。

关闭时使用 `destroy` 还是优雅 `end` 取决于错误类型；协议错误通常应立即终止，正常 close 可以先回 close 帧再结束。当前实现偏向简单可靠地释放资源。

### `listen()`

返回对象中的 `listen` 把 Node 回调式监听包装成 Promise。调用者可以 `await` 服务真正就绪，并取得 `server.address()`；测试传入端口 0 时，操作系统分配的随机端口也能被准确返回。监听错误会拒绝 Promise，而不是变成未捕获事件。

### `close()`

该函数先销毁所有记录中的活动浏览器和后端 socket，再 Promise 化关闭 HTTP server。只调用 `server.close()` 会停止接受新连接，却可能被现有长连接一直拖住，因此活动集合是测试不遗留句柄、进程能够退出的关键。

### 直接执行入口

文件末尾比较 `process.argv[1]` 与当前模块路径。只有 `node gateway.mjs` 直接运行时才创建实例并监听；被测试 import 时不产生端口副作用。启动失败会打印错误并设置非零退出状态，便于 Shell 或容器判断网关是否就绪。

## 面试重点

重要性最高。常见问题：为什么浏览器不能直接连原 TCP；WebSocket mask、ping/pong、upgrade 握手如何工作；两层半包如何分别缓存；为何使用静态白名单；如何处理背压。要说明当前未实现 WebSocket 分片和显式 `socket.write` 背压控制，生产扩展可使用成熟库和 pause/drain。
