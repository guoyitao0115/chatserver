# 问答整理：Buffer、epoll 底层流程、LT/ET 模式

> 本文整理了近期三条关键问答，便于复习和面试表达。

---

## 一、`chatserver.cpp` 里 `onMessage(conn, buffer, time)` 的 `buffer` 是什么？

### 1）这个 `buffer` 是什么

`buffer` 是 muduo 传入的**应用层读缓冲区**（`TcpConnection` 的 input buffer）。
里面保存的是：

- 该连接已经从 socket 读到用户态
- 但还没被业务层完全消费的字节流

在本项目中，`FrameCodec::decode()` 就是在这个 buffer 上做拆包。

### 2）是不是每个连接都有一个 `buffer`

是的。每个 `TcpConnection` 都有独立的 input/output buffer，不是全局共用。

### 3）这个 `buffer` 在内核空间吗

不是。它在**用户态进程内存**中。

典型路径：

1. 数据先到内核 socket 接收缓冲区（内核态）
2. muduo 调 `read/recv` 把数据拷贝到用户态 input buffer
3. 再调用 `onMessage` 给业务层处理

### 4）有大小限制吗

有两层：

- 内核 socket 接收缓冲区：受系统和 `SO_RCVBUF` 等参数限制
- 用户态 muduo Buffer：动态扩展，受进程内存上限限制

本项目还有应用层单帧限制：

- `FRAME_MAX_PAYLOAD = 4MB`

即单帧 payload 超过 4MB 会被拒绝并断连。

---

## 二、结合 epoll 讲这个项目的数据处理底层过程

以“客户端发来一条消息”为例：

1. 网卡收包，内核 TCP 协议栈重组后写入该连接的 socket 接收缓冲区。
2. 内核发现 fd 变为可读，将事件放入 epoll 的就绪队列。
3. 业务线程阻塞在 `epoll_wait`，被唤醒并拿到可读事件（`EPOLLIN`）。
4. muduo 在读事件处理里调用 `read/recv`，把数据从内核态搬运到用户态 input buffer。
5. muduo 调用你注册的 `onMessage(conn, buffer, time)` 回调。
6. `FrameCodec::decode()` 尝试拆帧：
   - 头不完整：返回等待下次
   - 头完整但体不完整：返回等待下次
   - 帧完整：提取 payload，回调 `onFrameMessage`，进入 `ChatService`。

### 关键纠正

`epoll` 本身只负责**通知“可读/可写”**，不负责数据搬运。  
真正的内核态 -> 用户态拷贝发生在 `read/recv` 系统调用中。

---

## 三、这个项目里的 epoll 是 LT 还是 ET？

结论：**LT（Level Triggered，水平触发）**。

理由：

1. 项目使用 `muduo::TcpServer` 默认 epoll 注册路径。
2. 当前代码没有显式设置 `EPOLLET`（ET 标志）。
3. 默认行为下，fd 只要还有未读数据，`epoll_wait` 会持续返回可读事件，这符合 LT 语义。

对比：

- **LT**：有数据没读完会继续通知。
- **ET**：只在状态变化时通知一次，需要循环读到 `EAGAIN`。

本项目当前运行特征与 LT 一致。

---

## 四、补充一句（和本项目改造相关）

本项目不会出现“阻塞读卡死”，但会有“慢速半包长期占用资源”风险。  
已通过方案A加入“半包超时断连”来治理该问题。
