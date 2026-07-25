# `friendmodel.cpp` 讲解

## 作用概览

**好友数据访问实现。** 写入双向好友边并查询对方用户资料。查询结果被登录响应序列化，使客户端能立即建立会话列表。

阅读位置：`src/server/model/friendmodel.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-24 行

```cpp
#include "friendmodel.hpp"
#include "db.h"
#include <cstdio>
#include <cstdlib>

/**
 * @brief 在 friend 关系表中记录“userid 关注 friendid”。
 *
 * 两个参数均是整数，通过 snprintf 写入固定长度缓冲区，不会把用户
 * 提供的字符串直接拼入 SQL。连接或写入失败由 MySQL 封装记录错误。
 *
 * @note 当前表达的是单向关系；若业务要求双向好友，应由事务同时写入
 * 两条关系，并依赖唯一索引防止重复。
 */
void FriendModel::insert(int userid, int friendid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "insert into friend values(%d, %d)", userid, friendid);

    MySQL mysql;
    if (mysql.connect())
    {
        mysql.update(sql);
    }
```

添加好友写入两个方向的关系，使 Alice 查询到 Bob 的同时 Bob 也能查询到 Alice。两条 INSERT 当前不是事务，第二条失败可能形成单向关系，这是面试中应主动指出的改进点。

### 片段 2：第 25-57 行

```cpp
}

/**
 * @brief 查询指定用户的好友基本信息。
 * @return 按数据库返回顺序组装的 User 列表；连接或查询失败时返回空列表。
 *
 * SQL 先从 friend 表找到 friendid，再与 user 表连接取出 id/name/state。
 * MYSQL_RES 由本函数拥有，遍历完成后显式释放，而连接由 MySQL 析构函数回收。
 */
vector<User> FriendModel::query(int userid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql),
             "select a.id,a.name,a.state from user a "
             "inner join friend b on b.friendid = a.id where b.userid=%d",
             userid);

    vector<User> vec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                User user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setState(row[2]);
                vec.push_back(user);
            }
```

连接 friend 与 user 表读取好友详情，避免关系表复制名称和在线状态。结果顺序由数据库返回，业务层将每个 User 转成登录响应中的嵌套 JSON 字符串。

查询成功后逐行读取结果集，把 SQL 列转换为领域对象或字符串集合。代码只在结果非空时访问列，并在结束后释放结果集；返回空集合既可能表示没有数据，也可能伴随日志中的查询失败，需要上层结合语义处理。

### 片段 3：第 58-62 行

```cpp
            mysql_free_result(res);
        }
    }
    return vec;
}
```

结果集在所有行转换完成后立即释放，再返回已经拥有自身字符串/数值副本的容器。这样后续 SQL 可以复用连接，也不会让领域对象持有指向 MYSQL_RES 内部缓冲的悬空指针。

## 面试重点

- 领域对象与数据访问层如何分工，业务层为什么不直接拼 SQL？

- 当前转义拼接、双向好友写入或群成员 N+1 查询有哪些一致性与性能改进空间？
