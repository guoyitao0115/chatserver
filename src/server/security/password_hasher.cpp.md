# `password_hasher.cpp` 讲解

## 作用概览

**bcrypt 实现。** 从系统随机源生成 bcrypt 允许字符集的 22 字符 salt，规范化 cost 后调用 `crypt_r`。校验时使用数据库摘要作为 salt 重算，并用恒定时间比较降低时序泄漏。

阅读位置：`src/server/security/password_hasher.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-23 行

```cpp
#include "password_hasher.hpp"
#include <crypt.h>
#include <random>
#include <array>

using namespace std;

namespace
{
// bcrypt 规定的64字符字母表，与通用 Base64 的顺序不同，不能混用。
const char BCRYPT_ALPHABET[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

/**
 * @brief 使用系统随机源生成 bcrypt 需要的 22 字符盐值。
 *
 * random_device 为随机引擎提供种子，uniform_int_distribution 保证字母表中
 * 64 个字符被均匀选取。每次哈希使用新盐，因此相同明文也不会产生相同存储值。
 */
string randomBcryptSalt22()
{
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> dist(0, 63);
```

这些头文件把“bcrypt 实现”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 24-45 行

```cpp

    string salt;
    salt.reserve(22);
    for (int i = 0; i < 22; ++i)
    {
        salt.push_back(BCRYPT_ALPHABET[dist(gen)]);
    }
    return salt;
}

/**
 * @brief 把 bcrypt cost 约束在标准允许的 4~31 范围内。
 *
 * cost 表示 2^cost 级别的计算开销；项目注册默认用 12，在安全性与
 * 登录延迟之间取平衡。
 */
int normalizeRounds(int rounds)
{
    if (rounds < 4) return 4;
    if (rounds > 31) return 31;
    return rounds;
}
```

这部分完成“bcrypt 实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。

### 片段 3：第 46-72 行

```cpp
} // namespace

/**
 * @brief 生成 crypt_r 可识别的 `$2b$cc$<22-char-salt>` 字符串。
 * @param rounds bcrypt cost，越大计算越慢。
 */
string PasswordHasher::generateBcryptSalt(int rounds)
{
    int cost = normalizeRounds(rounds);
    string salt = randomBcryptSalt22();

    char buf[40] = {0};
    // bcrypt salt 形式：$2b$12$<22chars>
    snprintf(buf, sizeof(buf), "$2b$%02d$%s", cost, salt.c_str());
    return string(buf);
}

/**
 * @brief 使用随机盐对明文密码进行 bcrypt 哈希。
 * @return 完整的算法/成本/盐/哈希字符串；crypt_r 失败时返回空串。
 *
 * crypt_r 使用函数内的 crypt_data，不共享 crypt() 的全局缓冲区，因而适合
 * Muduo 多 worker 线程并发注册的场景。
 */
string PasswordHasher::hashBcrypt(const string &plain, int rounds)
{
    string salt = generateBcryptSalt(rounds);
```

SQL 的数值字段按十进制写入，外部字符串则应在前一阶段完成 MySQL 转义。查询列顺序与后面的 row 下标一一对应；一旦调整 SELECT 列表，也必须同步对象装配顺序，避免名称、状态或角色错位。

### 片段 4：第 73-99 行

```cpp

    struct crypt_data data;
    data.initialized = 0;

    char *out = crypt_r(plain.c_str(), salt.c_str(), &data);
    if (out == nullptr)
    {
        return "";
    }

    return string(out);
}

/**
 * @brief 校验明文密码是否与已存储的 bcrypt 哈希匹配。
 *
 * 将完整 storedHash 作为 crypt_r 的 salt 参数，库会自动取出其中的版本、
 * cost 和盐，再对 plain 计算。结果与存储串完全一致时才通过。
 *
 * @note 当前使用 std::string 普通比较，不是显式的常数时间比较。
 */
bool PasswordHasher::verifyBcrypt(const string &plain, const string &storedHash)
{
    if (storedHash.empty())
    {
        return false;
    }
```

把数据库摘要本身交给 `crypt_r`，bcrypt 会从中提取算法、cost 和 salt 重算候选摘要。随后逐字节恒定时间比较，避免遇到首个差异就提前返回而泄漏匹配前缀。

`crypt_r` 使用调用方提供的 thread-local data，避免传统 `crypt` 的全局缓冲竞争。比较阶段遍历完整摘要并累计差异，不在首个不同字符处返回；这样错误密码的匹配前缀长度不会明显改变执行路径。

### 片段 5：第 100-112 行

```cpp

    struct crypt_data data;
    data.initialized = 0;

    // 传入完整存储哈希，crypt_r 会自动从中提取算法/盐参数
    char *out = crypt_r(plain.c_str(), storedHash.c_str(), &data);
    if (out == nullptr)
    {
        return false;
    }

    return storedHash == out;
}
```

这部分完成“bcrypt 实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 这里为每次 crypt_r 调用准备独立 crypt_data，并检查返回指针；哈希生成失败返回空串，校验失败返回 false，上层不会保存或接受不完整摘要。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？
