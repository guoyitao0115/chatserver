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
    return static_cast<int>(parsed);
}

} // namespace config
} // namespace chatserver

#endif
