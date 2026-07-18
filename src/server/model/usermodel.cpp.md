# `usermodel.cpp` 讲解

## 作用概览

该文件实现注册、登录查询、密码哈希升级及用户状态维护。

## 按学习顺序讲解

- `insert(user)`：连接数据库，转义昵称与密码哈希，插入后用自增 ID 更新对象。
- `query(id)`：按整数 ID 查询一行，构造包含密码字段的 `User` 供认证使用；不存在时返回默认对象。
- `updatePassword(id,pwdHash)`：转义新 bcrypt 哈希并更新指定用户，服务于旧明文平滑迁移。
- `updateState(user)`：转义 online/offline 状态并更新目标 ID。
- `resetState()`：把所有 online 行批量改为 offline，供服务退出恢复状态。

## 函数详细说明

### `UserModel::insert(user)`

这个函数用于注册用户。参数是 `User&`，因为函数需要读取昵称、密码哈希、初始状态，也要在插入成功后把自增 ID 写回对象。

实现先连接数据库，然后分别为 name、password、state 分配转义缓冲区，并调用 `mysql_real_escape_string`。虽然密码已经是 bcrypt 哈希，仍然做转义是正确的，因为哈希字符串里包含 `$`、`.`、`/` 等特殊字符，统一转义可以避免 SQL 语法问题。

SQL 插入成功后，函数通过 `mysql_insert_id` 获取新用户 ID，并 `user.setId(...)` 回填。返回 true 表示注册成功，false 表示连接或写入失败。业务层根据这个返回值决定给客户端返回注册成功还是失败。

### `UserModel::query(id)`

这个函数按用户 ID 查询一条用户记录。登录流程会先调用它取出数据库中的密码字段和当前状态，再进行密码校验和登录状态处理。

实现用 `snprintf` 组装 `select * from user where id = %d`，因为 id 是整数，注入风险低。查询成功后读取第一行，构造 `User`，填充 ID、昵称、密码和状态，然后释放结果集并返回。

如果连接失败、查询失败、没有记录，函数返回默认 `User()`。业务层通常通过 `user.getId() == -1` 判断不存在。这里要注意，返回对象里包含密码哈希，只能服务端内部使用，不能放进响应 JSON。

### `UserModel::updatePassword(id, pwdHash)`

这个函数更新指定用户的密码哈希，主要用于旧明文密码的平滑迁移。登录时如果发现数据库里的密码不是 bcrypt 格式，但明文校验成功，就会生成 bcrypt 哈希并调用此函数写回。

实现连接数据库后只转义 `pwdHash`，再拼接 `update user set password = ... where id = ...`。返回值直接来自 `mysql.update(sql)`。

这个设计可以做到“不停机迁移”：老用户第一次成功登录后自动升级，后续就走 bcrypt 校验。但它要求登录逻辑非常谨慎，不能在校验失败时更新，也不能把明文再次写入数据库。

### `UserModel::updateState(user)`

这个函数更新用户在线状态。虽然参数是完整 `User` 对象，实际用到的是 `user.getState()` 和 `user.getId()`。

实现上对状态字符串做转义，然后执行 `update user set state = ... where id = ...`。登录成功会写 `online`，退出或异常断线会写 `offline`。

它更多服务于展示和兼容旧逻辑。当前项目真正做跨节点在线路由时依赖 Redis 的 `chat:user:server:<userid>`，因为数据库状态没有 TTL，也不能表达“用户在哪个节点”。

### `UserModel::resetState()`

这个函数把所有 `online` 用户批量改成 `offline`，通常在服务进程退出时调用。它能修复“服务停了但数据库仍显示在线”的问题。

实现非常直接：执行 `update user set state = 'offline' where state = 'online'`。因为没有 serverId 条件，多节点部署时一个节点退出可能影响其他节点的在线用户，这是当前实现的局限。

更稳的设计是数据库也记录用户所在 serverId，并只重置本节点用户；或者完全以 Redis TTL 作为在线判断来源，数据库状态只作为弱展示字段。

## 面试重点

重要性中等。可能问题：旧明文如何无停机迁移？登录先识别格式，明文校验成功后写 bcrypt；批量 reset 在多节点是否安全？一个节点退出不应重置其他节点用户，当前实现有这个局限，更好是以 Redis TTL 为准或按 serverId 维护状态。
