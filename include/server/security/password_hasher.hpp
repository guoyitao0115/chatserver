#ifndef PASSWORD_HASHER_H
#define PASSWORD_HASHER_H

#include <string>

using std::string;

class PasswordHasher
{
public:
    // 生成 bcrypt 哈希（含随机盐）
    // rounds 范围建议 [4, 31]，常用 10~12
    static string hashBcrypt(const string &plain, int rounds = 12);

    // 校验明文是否匹配存储哈希
    static bool verifyBcrypt(const string &plain, const string &storedHash);

private:
    static string generateBcryptSalt(int rounds);
};

#endif