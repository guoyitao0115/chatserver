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
