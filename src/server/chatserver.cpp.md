# `chatserver.cpp` 讲解

## 作用概览

这是网络接入层实现。它把 Muduo 的连接和字节流事件转换为经过边界校验的完整 JSON 消息，并负责空闲连接、慢速半包和异常断线治理。

## 按学习顺序讲解

### `ChatServer::ChatServer(...)`

初始化 `TcpServer` 和 `FrameCodec`，绑定连接、消息和完整帧回调，配置 I/O 线程数，并在 EventLoop 注册周期巡检任务。

### `start()`

输出启动日志并调用 Muduo `TcpServer::start`。

### `onConnection(conn)`

连接建立时把当前时间写入活动表；断开时从活动表和半包表删除，调用 `ChatService::clientCloseException` 清理用户连接、Redis 路由和数据库状态。

### `onMessage(conn,buffer,time)`

每次收到字节先刷新活动时间，再调用 codec 解帧。解码后观察 Buffer：只有合法长度头已到、但完整 payload 尚未到时才记为半包；完整、空缓冲或非法帧都清除半包计时。

### `onFrameMessage(conn,payload,time)`

解析 JSON 对象并检查整数 `msgid`，从 `ChatService` 取得 handler。格式错误返回 `ERROR_MSG code=400`；捕获 JSON 异常并限制日志中的 payload 长度。

### `checkIdleConnections()`

遍历活动表，找出 45 秒未活动连接；锁内移除并收集强引用，锁外调用 `shutdown`。

### `checkPartialFrameTimeouts()`

找出持续 10 秒的半包，采用相同的“锁内收集、锁外断连”模式，防止慢速发送者长期占用资源。

## 函数详细说明

### `ChatServer::ChatServer(EventLoop *loop, const InetAddress &listenAddr, const string &nameArg)`

这是网络层的组装入口。构造函数先保存事件循环并创建 Muduo `TcpServer`，再创建 `FrameCodec`，把“成功解析出完整业务帧后的回调”绑定到 `onFrameMessage`。随后分别注册连接事件回调和原始字节到达回调，因此数据会按“TCP 字节流 → `onMessage` → codec 解帧 → `onFrameMessage` → 业务 handler”的路径流动。

构造阶段还设置 I/O 线程数，并通过 `EventLoop::runEvery` 注册半包与空闲连接巡检。周期任务运行在事件循环体系中，不需要额外创建永久轮询线程。这里需要注意对象生命周期：周期回调捕获了 `this`，因此 `ChatServer` 必须至少活到事件循环结束。

### `start()`

该函数负责输出监听信息并调用 `_server.start()`，让 Muduo 创建并启动 I/O 线程池、开始接受连接。它不会调用 `loop()`，事件循环仍由 `main.cpp` 统一启动，这样网络组件和进程生命周期的职责保持分离。

### `onConnection(const TcpConnectionPtr &conn)`

该函数同时处理连接建立和连接关闭。建立连接时，以 `conn->name()` 为键记录最后活动时间和连接的弱引用，为空闲检测提供数据。使用弱引用可避免监控表无意中延长连接对象生命周期。

连接关闭时，函数会从活动表和半包计时表删除对应记录，然后调用 `ChatService::clientCloseException`。后一步非常关键：TCP 断开不等于业务已经正常注销，服务层还必须移除本地用户—连接映射、条件删除 Redis 路由并修正数据库在线状态。清理接口按连接反查用户，避免依赖客户端在断线前主动发送注销消息。

### `onMessage(const TcpConnectionPtr &conn, Buffer *buffer, Timestamp time)`

这是 TCP 字节流进入应用层的第一站。每次读事件发生时先刷新连接最后活动时间，因此正常业务消息和持续接收数据都能阻止空闲超时。随后检查缓冲区是否已经包含完整的 4 字节长度头：只有长度合法、正文尚未收全时才开始或保留半包计时；缓冲为空、帧已经完整或长度非法时会清除半包状态。

之后调用 `_codec.decode`。codec 会循环处理缓冲区中的完整帧，所以既能把一次到达的多个粘连帧逐个交付，也会把尚未到齐的拆包数据留在 Muduo Buffer 中。解码完成后再次检查剩余缓冲，更新半包状态。这一前一后检查使“本次读事件刚好补齐上一帧，同时又带来下一帧的一部分”也能被正确计时。

共享表只在查询或更新时短暂加锁，真正的 JSON 解析和业务调用不在锁内执行。长度合法性最终仍由 codec 负责，网络层的预检查主要服务于半包超时判断。

### `onFrameMessage(const TcpConnectionPtr &conn, const string &payload, Timestamp time)`

该函数只接收 codec 已切分好的单个 JSON 正文。它先解析 JSON，确认顶层是对象且 `msgid` 为整数；验证通过后调用 `ChatService::getHandler(msgid)` 取得对应处理器，并把连接、JSON 对象和接收时间传入业务层。

如果 JSON 语法错误、顶层类型错误或缺少合法 `msgid`，函数会通过统一帧协议返回 `ERROR_MSG` 和 400 错误，而不是让异常越过 Muduo 回调边界导致进程退出。记录异常时只截取有限长度的 payload，既保留排错信息，也防止恶意大文本污染日志。这里完成的是协议级校验，登录身份、字段语义和权限仍由具体业务 handler 校验。

### `checkIdleConnections()`

周期任务比较当前单调时间与每条连接的最后活动时间，找出超过空闲阈值的连接。遍历时会尝试把弱引用提升为强引用：对象仍存在则加入待关闭列表，对象已经销毁则只清理残留记录。

函数采用“锁内判断和删除、锁外调用 `shutdown`”的两阶段方式。`shutdown` 可能触发更多网络事件和业务清理，若在互斥锁内执行容易造成长时间占锁甚至回调重入死锁。空闲超时主要回收已经不再通信的客户端资源，与半包超时针对的风险不同。

### `checkPartialFrameTimeouts()`

该函数检查连接从首次出现“合法长度头已到、正文未到齐”开始是否持续超过半包阈值。超时说明客户端长期占着一个未完成帧，可能是网络异常，也可能是 slowloris 式慢速发送。函数会删除计时记录并在锁外关闭连接，限制单连接长期占用 Buffer 和服务端状态。

半包计时不会因每次只补几个字节而无限刷新，它关注的是这一帧从进入不完整状态到现在的总时长；帧补齐、非法或缓冲清空后状态才会重置。因此它比单纯的“最近收到过数据”空闲检测更能约束慢速攻击。

## 面试重点

重要性最高。常见问题：Muduo Reactor 模型如何工作、粘包/拆包在哪层解决、如何防 4 MiB 以上恶意帧、空闲与半包超时分别防什么、为什么业务 handler 不在 codec 中。还可指出当前活动表按连接名加互斥锁，规模更大时可将连接状态绑定到连接上下文，减少全局表竞争。
