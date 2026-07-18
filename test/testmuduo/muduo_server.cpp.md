# `muduo_server.cpp` 讲解

## 作用概览

这是最小 Muduo echo 服务学习样例，用来理解 `TcpServer + EventLoop + 回调`，没有当前项目的帧协议、JSON 业务和可靠性逻辑。

## 按学习顺序讲解

- `ChatServer(loop,listenAddr,nameArg)`：组合 `TcpServer`，绑定连接与消息回调，设置 4 个线程。
- `start()`：启动监听。
- `onConnection(conn)`：打印连接建立/断开两端地址；断开时调用 shutdown。
- `onMessage(conn,buffer,time)`：一次性取出当前 Buffer 全部字节，打印后原样发送，是典型 echo；这种写法不能直接用于有消息边界的聊天协议。
- `main()`：监听 `127.0.0.1:6000`，启动服务并进入事件循环。

## 函数详细说明

### `ChatServer::ChatServer(EventLoop *loop, const InetAddress &listenAddr, const string &nameArg)`

构造函数用传入的事件循环、监听地址和服务名创建 `TcpServer`，再把连接事件与消息事件绑定到当前对象的成员函数。Muduo 接收到新连接或可读数据时会回调这些函数，业务代码无需自己编写 `accept`/`poll` 循环。

`setThreadNum(4)` 配置 I/O 线程池规模；主 loop 负责接受连接，已建立连接被分配到 I/O loop。这里的 4 是演示值，不是固定最优值，生产环境应根据 CPU 核数、连接负载和业务是否阻塞压测确定。

### `start()`

函数输出服务启动信息并调用 `TcpServer::start()`。start 会启动线程池并启用监听，但主事件循环仍需由 `main` 调用 `loop.loop()` 才能持续处理事件。把两步分开便于在进入循环前完成其他组件初始化。

### `onConnection(const TcpConnectionPtr &conn)`

回调通过 `conn->connected()` 区分建立和断开，并打印客户端、服务端地址以及连接状态。连接对象由智能指针管理，回调期间保证有效。断开分支中的 shutdown 在 echo 示例里主要用于演示接口，正式服务的重点应是清理用户映射、路由和业务状态。

### `onMessage(const TcpConnectionPtr &conn, Buffer *buffer, Timestamp time)`

函数调用 `retrieveAllAsString()` 取出当前 Buffer 的全部可读字节，打印后原样 `send` 回客户端，从而实现 echo。取出操作会推进 Buffer 读指针，因此这些字节不会被再次处理。

这种写法只适合不关心消息边界的演示。TCP 可能把一条 JSON 拆成多次回调，也可能一次回调带来多条 JSON；正式聊天服务必须先检查固定长度头，只取完整帧，并把不足部分留在 Buffer 中。

### `main()`

入口创建 `EventLoop` 和本地监听地址，构造 `ChatServer`，调用 `start` 后进入 `loop.loop()`。事件循环通常一直运行到进程退出，因此 server 对象在栈上的生命周期覆盖全部回调。

地址固定为 `127.0.0.1:6000`，只能本机访问且不支持配置，这再次说明它是学习样例；当前项目主服务会从参数或配置获取监听信息，并初始化数据库、Redis 和消息总线。

## 面试重点

重要性较低，但可用于解释学习路线。可能问题：为何正式服务不能 `retrieveAllAsString` 后直接 parse JSON？一次读可能只有半条或包含多条消息；正式版本用 4 字节长度头和累积 Buffer。
