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
