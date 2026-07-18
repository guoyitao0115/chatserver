# `password_hasher.cpp` 讲解

## 作用概览

该文件通过线程安全的 `crypt_r` 实现 bcrypt 盐生成、哈希和验证，并限制 cost 合法范围。

## 按学习顺序讲解

- `randomBcryptSalt22()`：匿名命名空间私有函数，用 `random_device + mt19937` 从 bcrypt 64 字符字母表生成 22 字符盐。
- `normalizeRounds(rounds)`：把 cost 限制在 bcrypt 允许的 4～31。
- `PasswordHasher::generateBcryptSalt(rounds)`：格式化为 `$2b$%02d$<salt>`。
- `hashBcrypt(plain,rounds)`：以新盐调用 `crypt_r`；失败返回空串。
- `verifyBcrypt(plain,storedHash)`：把完整存储哈希作为 salt 参数重算并比较。

## 函数详细说明

### `randomBcryptSalt22()`

这是匿名命名空间里的私有辅助函数，只在当前 `.cpp` 文件可见。它用于生成 bcrypt 需要的 22 位 salt 字符串。

实现上使用 `random_device` 作为随机源初始化 `mt19937`，再用均匀分布从 bcrypt 专用 64 字符字母表中抽取字符。循环 22 次后返回 salt。

需要注意，bcrypt 的字符表不是普通 Base64，开头是 `./`，后面才是大小写字母和数字。面试时可以补充：生产安全更推荐直接使用操作系统 CSPRNG 或成熟密码库，避免自己处理随机数细节。

### `normalizeRounds(rounds)`

这个函数把 bcrypt cost 限制在合法范围内。bcrypt 的 cost 通常允许 4 到 31，数值越大计算越慢，抗暴力破解能力越强，但登录耗时也越高。

如果传入值小于 4，函数返回 4；大于 31，返回 31；在范围内则原样返回。这样可以防止配置错误导致过低安全强度或过高 CPU 压力。

### `PasswordHasher::generateBcryptSalt(rounds)`

这个函数生成完整 bcrypt salt 前缀，格式类似 `$2b$12$xxxxxxxxxxxxxxxxxxxxxx`。其中 `$2b$` 是算法版本，`12` 是两位 cost，最后 22 位是随机 salt。

函数会先调用 `normalizeRounds`，再调用 `randomBcryptSalt22`，最后格式化成 `crypt_r` 可接受的 salt 字符串。注册和哈希函数依赖它来保证每个用户密码都有不同随机盐。

### `PasswordHasher::hashBcrypt(plain, rounds)`

这个函数用于把明文密码转换为 bcrypt 哈希。`plain` 是用户输入的密码，`rounds` 是 cost，默认通常使用 12。

执行流程是：生成新 salt，初始化 `crypt_data`，调用 `crypt_r(plain.c_str(), salt.c_str(), &data)`。返回值非空时，把结果字符串作为最终哈希返回；失败时返回空串。

bcrypt 的最终字符串已经包含算法版本、cost 和 salt，所以数据库只需要存这一整串，验证时不用额外存 salt。

### `PasswordHasher::verifyBcrypt(plain, storedHash)`

这个函数校验用户输入密码是否匹配数据库里的 bcrypt 哈希。关键点是直接把 `storedHash` 作为 `crypt_r` 的第二个参数；bcrypt 会从里面解析算法版本、cost 和 salt，然后重新计算。

重新计算出的哈希与 `storedHash` 完全相等，则说明密码正确。当前代码使用普通字符串比较，教学项目可以接受；如果追求更严谨的抗侧信道能力，可以改成恒定时间比较。

边界上，如果 `storedHash` 格式非法或 `crypt_r` 失败，函数会返回 false，登录自然失败。

## 面试重点

重要性高。常见问题：随机盐存在哪里？已包含在 bcrypt 哈希字符串内；cost 12 的意义？计算约为指数级增加；比较是否恒定时间？当前普通字符串比较不是严格恒定时间，可改用恒定时间比较。随机数实现也可进一步使用操作系统 CSPRNG。
