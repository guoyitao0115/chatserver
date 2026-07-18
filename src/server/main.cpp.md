# `main.cpp`（服务端）讲解

## 作用概览

这是 C++ 服务端进程入口：解析监听地址，安装退出信号处理器，创建 Muduo 事件循环与 `ChatServer` 并进入阻塞事件循环。

## 按学习顺序讲解

- `resetHandler(int)`：收到 Ctrl+C 的 `SIGINT` 后调用业务层 `reset` 把数据库在线状态置离线，再退出进程。
- `main(argc,argv)`：校验 IP/端口参数，注册信号处理，创建 `EventLoop`、`InetAddress` 和服务器，调用 `start` 后进入 `loop.loop()`。

## 函数详细说明

### `resetHandler(int)`

这是服务端收到 `SIGINT` 时的退出处理函数，也就是你在终端按 Ctrl+C 后会进入这里。参数是信号编号，当前实现没有使用，所以形参只保留类型。

执行流程很短：先调用 `ChatService::instance()->reset()`，把数据库里在线用户状态批量恢复为 `offline`；随后调用 `exit(0)` 结束进程。它的目的不是处理网络连接，而是避免服务进程退出后数据库仍残留在线状态，影响下一次登录或好友列表展示。

需要准确理解它的局限：POSIX 信号处理函数里调用复杂 C++ 逻辑、数据库操作、单例初始化都不是严格异步信号安全的。面试中可以说这是教学项目里常见的简化写法，工程化版本应让 signal handler 只写 pipe/eventfd 或设置原子标志，再由事件循环线程做优雅退出。

### `main(argc, argv)`

这是服务端进程入口。`argc` 和 `argv` 来自命令行，项目要求传入监听 IP 和端口，例如 `./ChatServer 127.0.0.1 6000`。如果参数不足，函数会打印示例并退出，避免服务以未知地址启动。

参数解析后，代码把 `argv[1]` 作为 IP，把 `argv[2]` 通过 `atoi` 转为端口。随后注册 `SIGINT` 处理器，创建 Muduo 的 `EventLoop`，再用 `InetAddress` 和 `ChatServer` 组装网络服务对象。

最后两步是 `server.start()` 和 `loop.loop()`：前者让 `TcpServer` 开始监听，后者进入 Muduo Reactor 事件循环并长期阻塞。也就是说，`main` 本身不处理业务消息，它只负责搭好事件循环和服务器对象，后续连接、收包、业务分发都交给 `ChatServer` 与 `ChatService`。

## 面试重点

重要性中等。可能问题：在 POSIX signal handler 中调用复杂 C++/数据库逻辑是否严格安全？不是异步信号安全；更稳妥做法是 self-pipe/eventfd 通知事件循环，在正常线程完成优雅退出。
