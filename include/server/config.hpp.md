# `config.hpp` 讲解

## 作用概览

**环境配置读取工具。** 把“环境变量存在且合法则覆盖，否则使用默认值”的规则封装起来，供数据库、Redis、RabbitMQ 和监听端口共享。

阅读位置：`include/server/config.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-25 行

```cpp
#ifndef CHAT_CONFIG_HPP
#define CHAT_CONFIG_HPP

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

namespace chatserver
{
namespace config
{

/**
 * 读取非空环境变量；变量不存在或值为空字符串时返回默认值。
 *
 * 该函数只复制进程环境中的内容，不保留 getenv() 返回指针，因此返回值不受后续
 * 环境变量修改影响。适合读取主机名、用户名、交换机名等字符串配置；若“空字符串”
 * 本身是合法配置，则不应使用此函数。
 */
inline std::string envOr(const char *name, const std::string &fallback)
{
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}
```

本片段读取 ``。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

### 片段 2：第 26-53 行

```cpp

/**
 * 读取并严格解析十进制整数环境变量。
 *
 * @param name 环境变量名，必须是有效的 C 字符串。
 * @param fallback 缺失、空值、格式错误、溢出或越界时采用的安全默认值。
 * @param minValue/maxValue 闭区间范围，默认适配 TCP/AMQP 端口。
 * @return 校验通过的 int，或 fallback；本函数不抛出配置解析异常。
 *
 * strtol 后同时检查 errno、是否消费到数字和是否完整消费字符串，因此 "12x"、
 * 空白尾缀以及超出 long 范围的值都不会被悄悄接受。调用者应保证 minValue 不大于
 * maxValue，并保证 fallback 本身符合业务约束。
 */
inline int envIntOr(const char *name, int fallback, int minValue = 1, int maxValue = 65535)
{
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        return fallback;
    }

    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minValue || parsed > maxValue)
    {
        return fallback;
    }
```

这一接口片段规定“环境配置读取工具”对外可用的操作和对象必须长期保存的状态。调用者只依赖这里的契约；锁、SQL、网络错误和资源释放留在实现内部，因此更换基础设施不会迫使业务处理器改写所有调用点。

### 片段 3：第 54-60 行

```cpp
    return static_cast<int>(parsed);
}

} // namespace config
} // namespace chatserver

#endif
```

这一接口片段规定“环境配置读取工具”对外可用的操作和对象必须长期保存的状态。调用者只依赖这里的契约；锁、SQL、网络错误和资源释放留在实现内部，因此更换基础设施不会迫使业务处理器改写所有调用点。 整数配置只有在完整解析且位于允许区间时才转换返回；尾随字符、溢出或越界都会使用 fallback，防止非法端口进入监听或连接调用。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？
