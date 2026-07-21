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
