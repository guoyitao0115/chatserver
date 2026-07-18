#include "password_hasher.hpp"
#include <crypt.h>
#include <random>
#include <array>

using namespace std;

namespace
{
const char BCRYPT_ALPHABET[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

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

int normalizeRounds(int rounds)
{
    if (rounds < 4) return 4;
    if (rounds > 31) return 31;
    return rounds;
}
} // namespace

string PasswordHasher::generateBcryptSalt(int rounds)
{
    int cost = normalizeRounds(rounds);
    string salt = randomBcryptSalt22();

    char buf[40] = {0};
    // bcrypt salt 形式：$2b$12$<22chars>
    snprintf(buf, sizeof(buf), "$2b$%02d$%s", cost, salt.c_str());
    return string(buf);
}

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
