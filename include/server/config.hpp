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

inline std::string envOr(const char *name, const std::string &fallback)
{
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

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
