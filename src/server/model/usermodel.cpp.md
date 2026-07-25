# `usermodel.cpp` 讲解

## 作用概览

**用户数据访问实现。** 将 `User` 与 `user` 表互相转换。写入字符串前使用 MySQL 转义，注册成功后把自增 id 回填对象，登录和断线流程依靠状态更新结果决定后续动作。

阅读位置：`src/server/model/usermodel.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-23 行

```cpp
#include "usermodel.hpp"
#include "db.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
using namespace std;

/**
 * @brief 创建用户，并回填数据库生成的用户 ID。
 * @param user 包含用户名、bcrypt 密码哈希和初始状态的领域对象。
 * @return INSERT 成功时返回 true，否则返回 false。
 *
 * 所有字符串字段均在已建立的 MySQL 连接上转义，既防止引号破坏
 * SQL 结构，也保证 bcrypt 中的特殊字符按原值保存。
 */
bool UserModel::insert(User &user)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }
```

注册对象的名称、bcrypt 摘要和状态先转义后插入。成功时用 mysql_insert_id 回填 user.id，业务层据此构造注册响应；失败则保留 id=-1，不会误报一个可登录账号。

### 片段 2：第 24-47 行

```cpp

    // 对字符串字段做转义，防止 SQL 注入/语法破坏
    vector<char> escName(user.getName().size() * 2 + 1);
    vector<char> escPwd(user.getPwd().size() * 2 + 1);
    vector<char> escState(user.getState().size() * 2 + 1);

    mysql_real_escape_string(mysql.getConnection(), escName.data(), user.getName().c_str(), (unsigned long)user.getName().size());
    mysql_real_escape_string(mysql.getConnection(), escPwd.data(), user.getPwd().c_str(), (unsigned long)user.getPwd().size());
    mysql_real_escape_string(mysql.getConnection(), escState.data(), user.getState().c_str(), (unsigned long)user.getState().size());

    string sql = "insert into user(name, password, state) values('";
    sql += escName.data();
    sql += "', '";
    sql += escPwd.data();
    sql += "', '";
    sql += escState.data();
    sql += "')";

    if (mysql.update(sql))
    {
        // 获取插入成功的用户数据生成的主键id
        user.setId(mysql_insert_id(mysql.getConnection()));
        return true;
    }
```

动态字符串先按当前 MySQL 连接的字符集执行转义，再拼入 SQL。例如名称含单引号时会作为字段内容保存，而不是提前结束字符串字面量；数值 id 则直接以十进制拼接。更大项目宜进一步改为预处理语句。

插入成功后读取当前连接产生的自增主键并回填领域对象。上层注册或建群响应因此拿到数据库真实 id，不需要再按名称查询一次，也避免同名并发导致取错记录。

### 片段 3：第 48-81 行

```cpp

    return false;
}

/**
 * @brief 按主键查询用户的认证与在线状态。
 * @return 命中时返回填充完整字段的 User；不存在或数据库失败时返回默认 User。
 *
 * 返回前先释放 MYSQL_RES，避免早退分支泄漏结果集。调用方通常以
 * user.getId() 是否等于查询 ID 来判断是否命中。
 */
User UserModel::query(int id)
{
    // 1.组装sql语句
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "select * from user where id = %d", id);

    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row != nullptr)
            {
                User user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setPwd(row[2]);
                user.setState(row[3]);
                mysql_free_result(res);
                return user;
            }
```

按主键最多读取一名用户并填充领域对象。查询不到时返回默认 id=-1 的 User，登录层用这个哨兵与请求 id 比较，统一处理“账号不存在”和“密码不正确”。

查询成功后逐行读取结果集，把 SQL 列转换为领域对象或字符串集合。代码只在结果非空时访问列，并在结束后释放结果集；返回空集合既可能表示没有数据，也可能伴随日志中的查询失败，需要上层结合语义处理。

### 片段 4：第 82-107 行

```cpp
            mysql_free_result(res);
        }
    }

    return User();
}

/**
 * @brief 更新用户密码字段，用于历史明文密码的平滑 bcrypt 迁移。
 *
 * 登录时若旧密码校验成功，ChatService 生成新哈希后调用本函数。
 * 哈希仍先转义再写入，函数返回值用于反映数据库是否成功接受升级。
 */
bool UserModel::updatePassword(int id, const string &pwdHash)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }

    vector<char> escPwd(pwdHash.size() * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(),
                             escPwd.data(),
                             pwdHash.c_str(),
                             (unsigned long)pwdHash.size());
```

历史明文用户首次成功登录后调用这里写回 bcrypt 摘要。只更新指定 id，摘要先转义；迁移失败不改变本次已验证身份，但会留下错误日志供后续修复。

### 片段 5：第 108-134 行

```cpp

    string sql = "update user set password = '";
    sql += escPwd.data();
    sql += "' where id = " + to_string(id);

    return mysql.update(sql);
}

/**
 * @brief 把 User 中的 online/offline 状态写回数据库。
 *
 * 状态字符串经转义后与整数 ID 拼接。调用者在登录失败时会根据返回值
 * 回滚 Redis 路由声明，避免路由已占用而数据库仍是离线的不一致。
 */
bool UserModel::updateState(User user)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }

    vector<char> escState(user.getState().size() * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(),
                             escState.data(),
                             user.getState().c_str(),
                             (unsigned long)user.getState().size());
```

在线状态更新只写指定用户的 state。登录必须检查返回值并在失败时回滚 Redis 路由，断线清理则通过条件路由删除避免旧连接覆盖新会话。

### 片段 6：第 135-159 行

```cpp

    string sql = "update user set state = '";
    sql += escState.data();
    sql += "' where id = " + to_string(user.getId());

    return mysql.update(sql);
}

/**
 * @brief 把数据库中所有 online 记录重置为 offline。
 *
 * 用于单节点服务启动/停止后清理残留状态。多节点场景下这是粗粒度
 * 操作，不能代替 Redis 中带服务器归属和 TTL 的实时路由。
 */
void UserModel::resetState()
{
    // 1.组装sql语句
    char sql[1024] = "update user set state = 'offline' where state = 'online'";

    MySQL mysql;
    if (mysql.connect())
    {
        mysql.update(sql);
    }
}
```

SQL 的数值字段按十进制写入，外部字符串则应在前一阶段完成 MySQL 转义。查询列顺序与后面的 row 下标一一对应；一旦调整 SELECT 列表，也必须同步对象装配顺序，避免名称、状态或角色错位。

## 面试重点

- 领域对象与数据访问层如何分工，业务层为什么不直接拼 SQL？

- 当前转义拼接、双向好友写入或群成员 N+1 查询有哪些一致性与性能改进空间？
