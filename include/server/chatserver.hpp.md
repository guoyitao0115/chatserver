# `chatserver.hpp` 讲解

## 作用概览

**网络层接口与连接状态。** 声明 Muduo 服务器回调、帧解码器以及半包/空闲连接巡检所需的时间表。它把 TCP 生命周期交给 `ChatServer`，把已解出的 JSON 业务交给 `ChatService`。

阅读位置：`include/server/chatserver.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-25 行

```cpp
#ifndef CHATSERVER_H
#define CHATSERVER_H

/*
 * ChatServer 是网络接入层，负责管理 Muduo TCP 服务和协议边界，不承载具体聊天业务：
 * - onMessage() 记录连接活跃度，并把字节流交给 FrameCodec 拆帧；
 * - onFrameMessage() 只接收完整 JSON，完成基础格式校验后按 msgid 分派给 ChatService；
 * - 定时巡检半包与空闲连接，限制慢速发送和失活会话占用资源。
 *
 * ChatServer 与传入 EventLoop 具有相同生命周期。构造完成后应在 EventLoop 所在线程
 * 调用 start()，随后由 Muduo 的 I/O 线程触发回调。业务发送统一使用 FrameCodec，
 * 不能绕过长度头直接发送裸 JSON。
 */

#include <muduo/net/TcpServer.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Logging.h>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <memory>
#include "framecodec.hpp"   // 帧编解码器
using namespace muduo;
using namespace muduo::net;
```

这些头文件把“网络层接口与连接状态”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 26-49 行

```cpp

// 聊天服务器的主类
class ChatServer
{
public:
    /**
     * 绑定监听地址、连接回调、字节流回调以及定时巡检任务。
     * loop 由外部拥有，必须非空且存活时间长于本对象；listenAddr/nameArg 分别用于
     * 创建监听 socket 和标识服务实例。构造函数只配置 TcpServer，不开始监听。
     */
    ChatServer(EventLoop *loop,
               const InetAddress &listenAddr,
               const string &nameArg);

    // 启动监听和 I/O 线程池；EventLoop::loop() 仍由 main 等外层代码负责运行。
    void start();

private:
    /**
     * TCP 连接建立/关闭回调。
     * 建立时登记最后活跃时间；关闭时清理半包、活跃度记录，并通知 ChatService 释放
     * 用户连接映射、在线路由和数据库状态。conn 是 Muduo 共享所有权句柄。
     */
    void onConnection(const TcpConnectionPtr &);
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

### 片段 3：第 50-73 行

```cpp

    /**
     * TCP 数据到达回调。每次读事件先更新活跃时间，再在解码前后维护半包起始时间，
     * 最后由 FrameCodec 从 Buffer 中按顺序取出 0..N 个完整帧。
     * Buffer 归 Muduo 所有，只能在回调期间访问；本函数不会把其裸指针保存到成员中。
     */
    void onMessage(const TcpConnectionPtr &,
                   Buffer *,
                   Timestamp);

    /**
     * 完整帧回调：解析 JSON 并验证 msgid 为整数，然后取得业务处理器同步调用。
     * JSON 格式错误会返回统一 400 错误帧，而不会让异常逃出 I/O 回调导致服务退出。
     * 这里只验证公共信封，用户身份和具体字段仍由 ChatService 校验。
     */
    void onFrameMessage(const TcpConnectionPtr &conn,
                        const string &payload,
                        Timestamp time);

    // 半包超时巡检：收集超过阈值的弱引用，释放互斥锁后再 shutdown，避免锁内回调。
    void checkPartialFrameTimeouts();

    // 空闲连接巡检：任何有效 TCP 流量都会刷新计时；超时后异步关闭连接。
    void checkIdleConnections();
```

这一接口片段规定“网络层接口与连接状态”对外可用的操作和对象必须长期保存的状态。调用者只依赖这里的契约；锁、SQL、网络错误和资源释放留在实现内部，因此更换基础设施不会迫使业务处理器改写所有调用点。

### 片段 4：第 74-95 行

```cpp

    TcpServer  _server;  // 拥有监听器和 I/O 线程池，析构时由 Muduo 清理底层资源
    EventLoop *_loop;    // 非拥有指针，仅用于注册定时任务；生命周期由启动入口保证

    // 帧编解码器：解决TCP粘包/拆包问题
    FrameCodec _codec;

    // 半包从首次观察到残留字节开始计时；持续补充少量数据不会无限刷新此期限。
    static constexpr int PARTIAL_FRAME_TIMEOUT_SEC = 10;
    // 正常客户端心跳周期为 10 秒，45 秒容忍多次抖动后才清理连接。
    static constexpr int IDLE_CONNECTION_TIMEOUT_SEC = 45;

    // key 使用 Muduo 唯一连接名；weak_ptr 避免巡检表延长已关闭连接的生命周期。
    unordered_map<string, pair<chrono::steady_clock::time_point, weak_ptr<TcpConnection>>> _partialFrameStart;
    // 记录最近一次收到流量的时间，用 steady_clock 避免系统时间校准造成误判。
    unordered_map<string, pair<chrono::steady_clock::time_point, weak_ptr<TcpConnection>>> _lastActivity;
    // 两张表分锁，降低数据接收与定时巡检相互阻塞；不得在持锁时执行网络关闭。
    mutex _partialFrameMutex;
    mutex _activityMutex;
};

#endif
```

互斥区保护 `共享状态` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

到这里，前面定义的组件被真正启动：监听器接管新连接，事件循环或异步任务持续运行。启动顺序保证回调和资源先准备好再接收流量，退出路径则负责释放连接或复位可恢复状态。

## 面试重点

- TCP 为什么必须自行处理半包和粘包，4 字节长度头如何完成增量解码？

- WebSocket 帧与后端 TCP 长度帧的边界分别在哪里，网关为什么不能承担最终鉴权？

- 最大帧长、半包超时和空闲超时各自防什么问题？
