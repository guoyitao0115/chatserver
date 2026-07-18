# `chatserver.hpp` 讲解

## 作用概览

该文件声明网络接入层 `ChatServer`。它管理 Muduo TCP 服务、帧解码、连接活动时间和半包超时，但不承载具体聊天业务。

## 按学习顺序讲解

### `ChatServer(loop, listenAddr, nameArg)`

构造服务器并注册连接、读事件与完整帧回调，同时安排周期性空闲/半包巡检。

### `start()`

启动 Muduo 的监听和 I/O 线程。

### `onConnection(conn)`

处理连接建立/断开：维护活动记录，断开时清理状态并通知 `ChatService` 释放登录用户。

### `onMessage(conn, buffer, timestamp)`

记录最新活动时间，把 Muduo Buffer 交给 `FrameCodec` 尽可能拆出完整帧，并根据剩余数据更新“半包开始时间”。

### `onFrameMessage(conn, payload, timestamp)`

只接收完整 JSON 负载；校验 `msgid` 后从业务层取得 handler。解析失败返回统一 400 错误，不让异常终止 I/O 线程。

### `checkIdleConnections()`

关闭超过 45 秒无业务/心跳的连接。先在锁内收集目标，锁外断开，缩短临界区。

### `checkPartialFrameTimeouts()`

关闭连续 10 秒仍未收齐的帧，防御慢速连接长期占用内存和连接资源。

## 函数详细说明

### `ChatServer(EventLoop *loop, const InetAddress &listenAddr, const string &nameArg)`

- **参数**：事件循环必须在服务器生命周期内有效；监听地址决定绑定 IP/端口；名称用于 Muduo 日志与连接命名。
- **初始化职责**：构造 `TcpServer`，把 codec 的完整帧回调绑定到 `onFrameMessage`，注册连接和读事件回调，并安排空闲连接与半包周期巡检。
- **对象关系**：`ChatServer` 拥有 `TcpServer` 和 `FrameCodec`，但只借用 `EventLoop*`。

### `start()`

- **输入/返回**：无参数、无返回。
- **副作用**：启动 Muduo 的监听 socket 和线程池。事件循环仍需由进程入口调用 `loop.loop()` 才会持续处理事件。
- **调用位置**：仅由服务端 `main` 在所有回调配置完成后调用。

### `onConnection(const TcpConnectionPtr &conn)`

- **建立分支**：记录连接最近活动时间和弱引用，供 45 秒空闲巡检。
- **断开分支**：删除活动、半包跟踪项，再通知业务层清理用户连接、Redis 路由和数据库状态。
- **并发点**：两个跟踪表分别有互斥锁；弱引用避免巡检表意外延长连接生命周期。

### `onMessage(conn, Buffer *buffer, Timestamp time)`

- **输入**：可能是半个帧、一个完整帧或多个粘连帧，不能假设一次回调等于一条消息。
- **流程**：刷新活动时间 → codec 尽量解码 → 检查剩余 Buffer 是否为“合法长度头已到但正文不足” → 新增或清除半包起始时间。
- **副作用**：可能触发零到多次业务处理；不会把不完整正文交给 JSON 解析器。
- **安全性**：半包计时只在真正不完整时开始，避免正常空 Buffer 被误判。

### `onFrameMessage(conn, payload, time)`

- **输入**：codec 已保证 payload 边界完整，但内容仍可能不是合法 JSON。
- **流程**：解析 JSON → 要求顶层是对象且 `msgid` 是整数 → 获取 handler → 执行业务函数。
- **错误路径**：结构错误或解析异常均返回 `ERROR_MSG`、HTTP 风格 400 码；日志只截取 payload 前 200 字符，避免异常大日志。
- **分层**：该函数是网络层进入业务层的唯一主要入口。

### `checkIdleConnections()`

- **判断依据**：当前稳态时钟减去最后活动时间达到 45 秒。
- **锁策略**：锁内遍历、删除并把可用弱引用提升成强引用；锁外执行 `shutdown`，避免断连回调重入时持锁。
- **结果**：客户端业务流量或心跳都会刷新活动时间；真正失联连接最终被释放。

### `checkPartialFrameTimeouts()`

- **判断依据**：连接从首次进入半包状态起超过 10 秒仍未补齐。
- **处理**：锁内收集连接并删除记录，锁外断开。即使弱引用已失效也清理表项。
- **防护目标**：应对慢速发送/异常客户端长期只发长度头或少量正文，而不是替代普通空闲超时。


## 面试重点

重要性高。常见问题：为什么网络层与业务层分开？便于协议解析、并发模型和业务规则独立演进；为何成员表保存 `weak_ptr`？巡检表不应延长连接生命周期；为什么不能在持锁时 `shutdown`？可能触发回调并放大锁竞争或死锁风险。
