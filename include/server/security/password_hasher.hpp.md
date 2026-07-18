# `password_hasher.hpp` 讲解

## 作用概览

该接口集中提供 bcrypt 密码哈希和校验，业务层永远不需要了解盐格式或 `crypt_r` 细节。

## 按学习顺序讲解

- `hashBcrypt(plain, rounds)`：生成随机盐并计算带算法、cost 和盐的完整 bcrypt 字符串；默认 cost 12。
- `verifyBcrypt(plain, storedHash)`：使用存储哈希中的算法/盐重新计算并比较。
- `generateBcryptSalt(rounds)`：私有辅助函数，规范 cost 并生成 `$2b$cc$...` 格式盐。

## 函数详细说明

### `static string hashBcrypt(const string &plain, int rounds)`

- **输入**：明文密码和 cost；业务层在调用前限制 6～72 字节。
- **流程**：调用私有盐生成函数，再用 `crypt_r` 计算完整 bcrypt 字符串。
- **返回**：成功返回包含版本、cost、盐和摘要的字符串；底层失败返回空串，注册函数据此拒绝写库。
- **副作用**：不修改输入，也不保存全局盐；每次调用都应产生不同哈希。

### `static bool verifyBcrypt(const string &plain, const string &storedHash)`

- 空存储值直接 false。
- 把完整存储哈希作为 `crypt_r` 的 salt 参数，底层自动读取版本、cost 和盐，重算后比较。
- 返回 true 只代表密码匹配，不做用户状态检查。
- 当前字符串比较不是严格恒定时间；在更高安全要求下应使用恒定时间比较函数。

### `static string generateBcryptSalt(int rounds)`

私有函数先把 cost 归一到 4～31，再生成 22 字符随机盐，格式化为 `$2b$cc$<salt>`。只有哈希函数能调用它，避免业务层自己拼装不合法盐。


## 面试重点

重要性高。可能问题：为什么不能存明文或普通 SHA？密码哈希需要随机盐和可调成本抵御彩虹表与暴力破解；bcrypt 为什么限制密码长度？有效输入上限通常为 72 字节，本项目注册也做了对应限制。
