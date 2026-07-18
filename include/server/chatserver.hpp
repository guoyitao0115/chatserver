#ifndef CHATSERVER_H
#define CHATSERVER_H

/*
 * ChatServer：
 * - onMessage() 仅负责把 Buffer 交给 FrameCodec 拆帧
 * - onFrameMessage() 只处理完整 JSON 业务消息
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
    // 初始化聊天服务器对象
    ChatServer(EventLoop *loop,
               const InetAddress &listenAddr,
               const string &nameArg);

    // 启动服务
    void start();

private:
    // 上报链接相关信息的回调函数
    void onConnection(const TcpConnectionPtr &);

    // 上报读写事件相关信息的回调函数
    // 此回调仅负责将数据喂给 FrameCodec，不直接解析业务
    void onMessage(const TcpConnectionPtr &,
                   Buffer *,
                   Timestamp);

    // 完整帧到达后的业务处理（由 FrameCodec 回调）
    void onFrameMessage(const TcpConnectionPtr &conn,
                        const string &payload,
                        Timestamp time);

    // 半包超时巡检：超过阈值仍未收齐帧则断开连接
    void checkPartialFrameTimeouts();

    // 空闲连接巡检：客户端心跳或业务流量超时则主动断开。
    void checkIdleConnections();

    TcpServer  _server;  // 组合的muduo库，实现服务器功能的类对象
    EventLoop *_loop;    // 指向事件循环对象的指针

    // 帧编解码器：解决TCP粘包/拆包问题
    FrameCodec _codec;

    // 半包超时阈值（秒）
    static constexpr int PARTIAL_FRAME_TIMEOUT_SEC = 10;
    static constexpr int IDLE_CONNECTION_TIMEOUT_SEC = 45;

    // 连接 -> 半包首次出现时间 + 连接弱引用（用于慢速半包超时断连）
    unordered_map<string, pair<chrono::steady_clock::time_point, weak_ptr<TcpConnection>>> _partialFrameStart;
    unordered_map<string, pair<chrono::steady_clock::time_point, weak_ptr<TcpConnection>>> _lastActivity;
    mutex _partialFrameMutex;
    mutex _activityMutex;
};

#endif
