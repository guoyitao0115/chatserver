# bcrypt 密码哈希存储（含随机盐）问题

## 一、所解决的问题

本次改造解决的是：**用户密码明文存储**带来的严重安全风险。

改造前，注册接口收到密码后直接写入 `user.password` 字段；登录时直接与数据库值做明文比对。该方式在数据库泄露、备份泄露、日志误打点等场景下风险极高。

---

## 二、bcrypt（含随机盐）原理详解

bcrypt 的核心思想是：
1. 对密码做**不可逆哈希**，不保存明文；
2. 每次哈希都带**随机盐（salt）**，即使两个人密码相同，哈希结果也不同；
3. 使用**可调成本因子（cost/rounds）**，故意让计算变慢，提升暴力破解成本。

### 1）哈希格式
bcrypt 存储串通常类似：
- `$2b$12$<22位salt><hash>`

其中：
- `$2b$`：算法标识
- `12`：成本因子
- 后续部分包含盐和哈希结果

### 2）注册时做什么
- 生成随机盐；
- 用 `bcrypt(plain_password, salt)` 得到哈希串；
- 将哈希串入库（不是明文）。

### 3）登录时做什么
- 读取库中的哈希串（它已包含算法/盐/成本参数）；
- 用用户输入明文再次计算；
- 比较计算结果与存储串是否一致。

### 4）为什么“随机盐 + 慢哈希”重要
- 抵抗彩虹表（预计算表）攻击；
- 抵抗同密码同哈希的批量关联；
- 提高离线爆破成本。

---

## 三、原方案的缺陷

1. **明文直存**：数据库一旦泄露，密码立即暴露。
2. **缺少盐值**：相同密码可被直接比对识别。
3. **缺少慢哈希**：攻击者可高速批量尝试常见密码。
4. **不满足生产安全基线**：不符合常见安全审计要求。

---

## 四、修改后的优点

1. **密码不落明文**：泄露后无法直接还原密码。
2. **随机盐防关联**：同密码用户的哈希值也不同。
3. **成本可调**：可按机器性能平衡安全与性能。
4. **兼容平滑迁移**：历史明文用户可在登录后自动升级为 bcrypt。

---

## 五、修改了哪些代码（文件与行号）

## 1）新增密码哈希模块

### 文件
- `include/server/security/password_hasher.hpp`
- `src/server/security/password_hasher.cpp`

### 行号
- `include/server/security/password_hasher.hpp:8`：`class PasswordHasher`
- `include/server/security/password_hasher.hpp:13`：`hashBcrypt(...)`
- `include/server/security/password_hasher.hpp:16`：`verifyBcrypt(...)`
- `include/server/security/password_hasher.hpp:19`：`generateBcryptSalt(...)`

- `src/server/security/password_hasher.cpp:10`：bcrypt 字符表
- `src/server/security/password_hasher.cpp:12`：`randomBcryptSalt22()`
- `src/server/security/password_hasher.cpp:35`：`generateBcryptSalt(...)`
- `src/server/security/password_hasher.cpp:42`：生成 `$2b$cost$salt` 格式
- `src/server/security/password_hasher.cpp:46`：`hashBcrypt(...)`
- `src/server/security/password_hasher.cpp:53`：`crypt_r(...)` 计算哈希
- `src/server/security/password_hasher.cpp:62`：`verifyBcrypt(...)`
- `src/server/security/password_hasher.cpp:73`：`crypt_r(...)` 校验

### 详细讲解
- 使用 `crypt_r`（可重入）而非 `crypt`，避免多线程静态缓冲区问题。
- `hashBcrypt`：生成随机盐并输出完整 bcrypt 存储串。
- `verifyBcrypt`：将存储串直接作为 `salt` 参数传入，由系统库提取算法/盐/成本进行校验。

---

## 2）注册/登录链路改造

### 文件
- `src/server/chatservice.cpp`

### 行号
- `4`：引入 `password_hasher.hpp`
- `12`：新增 `isBcryptHash(...)` 辅助函数
- `195`：`login(...)` 入口
- `210`：若库中为 bcrypt，则 `verifyBcrypt(...)`
- `218`：历史明文账号标记 `needUpgradeToBcrypt`
- `249-254`：登录成功后自动升级为 bcrypt 并回写数据库
- `338`：`reg(...)` 入口
- `343`：注册时 `hashBcrypt(...)`

### 详细讲解
- **注册链路**：
  - 明文密码先哈希再入库；
  - 哈希失败则注册失败并记录日志。

- **登录链路**：
  - 若库中是 bcrypt 串：走 `verifyBcrypt`；
  - 若库中是历史明文：先明文比对；比对成功后立即升级为 bcrypt（平滑迁移）。

这种迁移策略无需一次性改库脚本，用户自然登录即可逐步完成升级。

---

## 3）用户模型新增密码更新接口

### 文件
- `include/server/model/usermodel.hpp`
- `src/server/model/usermodel.cpp`

### 行号
- `include/server/model/usermodel.hpp:16`：声明 `updatePassword(...)`
- `src/server/model/usermodel.cpp:72`：实现 `updatePassword(...)`
- `src/server/model/usermodel.cpp:88`：执行 `update user set password = ...`

### 详细讲解
- 为平滑迁移提供原子更新能力；
- SQL 仍沿用转义，避免注入与语法破坏。

---

## 4）构建系统接入 security 模块

### 文件
- `CMakeLists.txt`
- `src/server/CMakeLists.txt`

### 行号
- `CMakeLists.txt:17`：新增 `include/server/security`
- `src/server/CMakeLists.txt:7`：新增 `SECURITY_LIST`
- `src/server/CMakeLists.txt:10`：`SECURITY_LIST` 加入 `ChatServer`
- `src/server/CMakeLists.txt:12`：链接 `crypt`

### 详细讲解
- 新增 `security` 子模块，避免把密码逻辑散落在业务代码；
- 链接 `crypt` 以启用系统 bcrypt 能力。

---

## 六、最终效果

1. 新注册用户不再明文存储密码；
2. 历史明文用户可在登录成功后自动升级为 bcrypt；
3. 密码安全性明显提升，满足简历与工程实践中的主流安全要求；
4. 对现有业务接口侵入小，改造成本可控。