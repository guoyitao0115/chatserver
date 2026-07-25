# `main.cpp` 讲解

## 作用概览

**服务端进程入口。** 解析监听地址，创建 Muduo 事件循环和 ChatServer，注册退出信号清理数据库在线状态，然后进入事件循环。

阅读位置：`src/server/main.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-30 行

```cpp
#include "chatserver.hpp"
#include "chatservice.hpp"
#include <iostream>
#include <signal.h>
using namespace std;

/**
 * @brief SIGINT 信号处理入口，在服务器被 Ctrl+C 终止前恢复用户状态。
 *
 * ChatService::reset() 会把数据库中仍标记为 online 的用户统一改为
 * offline，避免正常停服后留下“假在线”记录。这是进程级的兼容性
 * 收尾逻辑；多节点部署中，用户实际路由仍以 Redis 的带 TTL 映射为准。
 *
 * @note 此处沿用原项目的信号处理方式；严格来说，数据库调用不是
 * async-signal-safe，生产环境更适合在信号中只设置退出标志，再由事件循环收尾。
 */
void resetHandler(int)
{
    ChatService::instance()->reset();
    exit(0);
}

int main(int argc, char **argv)
{
    // 启动参数固定为监听 IP 和端口，参数不完整时立即退出，避免带错误地址启动。
    if (argc < 3)
    {
        cerr << "command invalid! example: ./ChatServer 127.0.0.1 6000" << endl;
        exit(-1);
    }
```

服务启动时把数据库中遗留的 online 状态统一复位。进程异常退出无法逐个执行断线回调，若不清理，下一次投递会把用户误认为在线却找不到有效节点路由。

函数内静态对象把业务服务限制为进程内单例。所有网络回调共享同一份连接表和基础设施连接，避免不同 ChatServer 回调各自维护互相矛盾的在线状态。

### 片段 2：第 31-48 行

```cpp

    // 解析通过命令行参数传递的ip和port
    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    // 注册优雅停服的兼容性处理，使测试或手工停服后用户状态可恢复。
    signal(SIGINT, resetHandler);

    // EventLoop 负责 epoll 事件循环；ChatServer 在其上注册连接、消息与定时回调。
    EventLoop loop;
    InetAddress addr(ip, port);
    ChatServer server(&loop, addr, "ChatServer");

    server.start(); // 开始监听，并启动 Muduo 的 I/O 线程池。
    loop.loop();    // 进入阻塞事件循环，直到进程退出。

    return 0;
}
```

到这里，前面定义的组件被真正启动：监听器接管新连接，事件循环或异步任务持续运行。启动顺序保证回调和资源先准备好再接收流量，退出路径则负责释放连接或复位可恢复状态。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？
