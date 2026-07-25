# `password_hasher.hpp` 讲解

## 作用概览

**密码哈希接口。** 限定注册和历史密码迁移只能通过 bcrypt 生成摘要，并提供基于已有摘要的校验入口，避免业务层接触 salt 格式细节。

阅读位置：`include/server/security/password_hasher.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-24 行

```cpp
#ifndef PASSWORD_HASHER_H
#define PASSWORD_HASHER_H

#include <string>

using std::string;

/**
 * bcrypt 密码哈希工具。
 *
 * 输出包含算法版本、成本因子和随机盐，可直接存入 user.password。该类无状态，公开
 * 方法使用 crypt_r 的线程局部工作区，适合多 I/O 线程并发调用。接口以空字符串/
 * false 表示底层 crypt 失败；调用方不得在失败时退回明文存储。
 */
class PasswordHasher
{
public:
    /**
     * 为明文生成带随机盐的 bcrypt 哈希。
     * rounds 会被规范到 [4,31]；默认 12 在安全性和登录耗时之间取平衡。bcrypt 只
     * 使用密码前 72 字节，业务注册层必须限制长度，避免不同长密码得到相同有效输入。
     * @return 成功时为完整哈希，crypt_r 失败时为空字符串。
     */
    static string hashBcrypt(const string &plain, int rounds = 12);
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

### 片段 2：第 25-38 行

```cpp

    /**
     * 使用 storedHash 自带的版本、成本和盐重新计算后比较结果。
     * 空哈希、格式不受支持或 crypt_r 失败均返回 false。不要把该返回值用于区分
     * “用户不存在”和“密码错误”，以免认证响应泄露账号存在性。
     */
    static bool verifyBcrypt(const string &plain, const string &storedHash);

private:
    // 生成 "$2b$<cost>$<22字符盐>"；只供 hashBcrypt 使用，不是最终密码哈希。
    static string generateBcryptSalt(int rounds);
};

#endif
```

这一接口片段规定“密码哈希接口”对外可用的操作和对象必须长期保存的状态。调用者只依赖这里的契约；锁、SQL、网络错误和资源释放留在实现内部，因此更换基础设施不会迫使业务处理器改写所有调用点。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？
