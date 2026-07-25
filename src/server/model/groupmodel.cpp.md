# `groupmodel.cpp` 讲解

## 作用概览

**群组数据访问实现。** 先查询群基本信息，再查询每个群的成员；群聊时则排除发送者只返回目标 id。字符串字段在拼接 SQL 前转义，避免名称或描述中的引号破坏语句。

阅读位置：`src/server/model/groupmodel.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-25 行

```cpp
#include "groupmodel.hpp"
#include "db.h"
#include <vector>
#include <cstdio>

/**
 * @brief 写入群基本信息，并把数据库生成的群 ID 回填到 group。
 * @return 只有 INSERT 成功时返回 true。
 *
 * 群名与描述是外部输入，因此先使用当前连接的 mysql_real_escape_string
 * 处理引号、反斜杠等字符。转义缓冲区按“原长度两倍+1”分配，
 * 满足 MySQL C API 的最大膨胀空间。
 */
bool GroupModel::createGroup(Group &group)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }

    vector<char> escName(group.getName().size() * 2 + 1);
    vector<char> escDesc(group.getDesc().size() * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(), escName.data(), group.getName().c_str(), (unsigned long)group.getName().size());
    mysql_real_escape_string(mysql.getConnection(), escDesc.data(), group.getDesc().c_str(), (unsigned long)group.getDesc().size());
```

群名与描述转义后写入 allgroup，成功再把自增 group id 回填对象。ChatService 随后用这个 id 把创建者加入 groupuser 并标记 creator。

动态字符串先按当前 MySQL 连接的字符集执行转义，再拼入 SQL。例如名称含单引号时会作为字段内容保存，而不是提前结束字符串字面量；数值 id 则直接以十进制拼接。更大项目宜进一步改为预处理语句。

### 片段 2：第 26-57 行

```cpp

    string sql = "insert into allgroup(groupname, groupdesc) values('";
    sql += escName.data();
    sql += "', '";
    sql += escDesc.data();
    sql += "')";

    if (mysql.update(sql))
    {
        group.setId(mysql_insert_id(mysql.getConnection()));
        return true;
    }

    return false;
}

/**
 * @brief 把用户与群、群角色之间的关系写入 groupuser 表。
 *
 * userid/groupid 是整数，role 则先转义后拼接。创建者传 creator，
 * 普通成员传 normal。当前返回 void，上层无法直接区分“已加入”与“写入失败”。
 */
void GroupModel::addGroup(int userid, int groupid, string role)
{
    MySQL mysql;
    if (mysql.connect())
    {
        vector<char> escRole(role.size() * 2 + 1);
        mysql_real_escape_string(mysql.getConnection(),
                                 escRole.data(),
                                 role.c_str(),
                                 (unsigned long)role.size());
```

把 userId、groupId 和角色写入成员关系表。角色字符串也需要转义；数据库唯一约束负责阻止同一用户重复加入同一群。

插入成功后读取当前连接产生的自增主键并回填领域对象。上层注册或建群响应因此拿到数据库真实 id，不需要再按名称查询一次，也避免同名并发导致取错记录。

### 片段 3：第 58-86 行

```cpp

        char sql[1024] = {0};
        snprintf(sql, sizeof(sql),
                 "insert into groupuser values(%d, %d, '%s')",
                 groupid, userid, escRole.data());
        mysql.update(sql);
    }
}

/**
 * @brief 查询用户参加的所有群，并为每个群装载成员与角色。
 * @return Group 列表；每个 Group 内部包含 GroupUser 列表。
 *
 * 执行分两阶段：先联表获取群基本信息，再按 groupid 逐群查询成员。
 * 每个 mysql_use_result 结果都在发起下一条 SQL 前释放，满足流式结果集的资源约束。
 *
 * @note 该实现是经典 N+1 查询；群数较多时可改为一次联表查询后在内存分组。
 */
vector<Group> GroupModel::queryGroups(int userid)
{
    /*
    1. 先根据userid在groupuser表中查询出该用户所属的群组信息
    2. 在根据群组信息，查询属于该群组的所有用户的userid，并且和user表进行多表联合查询，查出用户的详细信息
    */
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql),
             "select a.id,a.groupname,a.groupdesc from allgroup a inner join "
             "groupuser b on a.id = b.groupid where b.userid=%d",
             userid);
```

SQL 的数值字段按十进制写入，外部字符串则应在前一阶段完成 MySQL 转义。查询列顺序与后面的 row 下标一一对应；一旦调整 SELECT 列表，也必须同步对象装配顺序，避免名称、状态或角色错位。

### 片段 4：第 87-108 行

```cpp

    vector<Group> groupVec;

    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            // 查出userid所有的群组信息
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                Group group;
                group.setId(atoi(row[0]));
                group.setName(row[1]);
                group.setDesc(row[2]);
                groupVec.push_back(group);
            }
            mysql_free_result(res);
        }
    }
```

先把用户所属群的基本行装成 Group 列表并释放第一份结果集，再逐群查询成员，把 id、名称、在线状态和角色追加到对应 Group。返回结构直接供登录响应使用；代价是群数为 N 时产生 1+N 条查询。

查询成功后逐行读取结果集，把 SQL 列转换为领域对象或字符串集合。代码只在结果非空时访问列，并在结束后释放结果集；返回空集合既可能表示没有数据，也可能伴随日志中的查询失败，需要上层结合语义处理。

### 片段 5：第 109-130 行

```cpp

    // 查询群组的用户信息
    for (Group &group : groupVec)
    {
        snprintf(sql, sizeof(sql),
                 "select a.id,a.name,a.state,b.grouprole from user a "
                 "inner join groupuser b on b.userid = a.id where b.groupid=%d",
                 group.getId());

        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                GroupUser user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setState(row[2]);
                user.setRole(row[3]);
                group.getUsers().push_back(user);
            }
```

SQL 的数值字段按十进制写入，外部字符串则应在前一阶段完成 MySQL 转义。查询列顺序与后面的 row 下标一一对应；一旦调整 SELECT 列表，也必须同步对象装配顺序，避免名称、状态或角色错位。 这段继续落实“群组数据访问实现”的当前分支，并把已确认结果交给紧接着的状态更新；失败路径不会伪装成成功响应。

### 片段 6：第 131-161 行

```cpp
            mysql_free_result(res);
        }
    }
    return groupVec;
}

/**
 * @brief 获取群聊的所有目标成员 ID，并排除发送者自己。
 *
 * ChatService::groupChat() 使用返回列表逐个调用统一投递逻辑，从而为
 * 本节点、其他节点与离线用户选择不同路由。结果集在本函数内完整释放。
 */
vector<int> GroupModel::queryGroupUsers(int userid, int groupid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql),
             "select userid from groupuser where groupid = %d and userid != %d",
             groupid, userid);

    vector<int> idVec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                idVec.push_back(atoi(row[0]));
            }
```

群聊只查询除发送者外的成员 id，并为每个成员独立调用统一投递。计数器区分本地、跨节点和离线路径；只要有成员投递失败，本次 ACK 就是失败且去重标记被撤销，因此重试可能让已成功成员收到重复，客户端仍需按 message_id 去重展示。

群聊只选择同群且 id 不等于发送者的成员。返回的每个 id 都是独立投递目标，发送者不会从服务器再收到一份自己的群消息。

群聊扇出集合不含发送者本人。每个成员独立走统一投递，route 计数用于日志观察本地、MQ 和离线比例；`allDelivered` 累积所有结果，不能因为前几个成员成功就提前给整条群消息成功 ACK。

### 片段 7：第 162-166 行

```cpp
            mysql_free_result(res);
        }
    }
    return idVec;
}
```

结果集在所有行转换完成后立即释放，再返回已经拥有自身字符串/数值副本的容器。这样后续 SQL 可以复用连接，也不会让领域对象持有指向 MYSQL_RES 内部缓冲的悬空指针。

## 面试重点

- 领域对象与数据访问层如何分工，业务层为什么不直接拼 SQL？

- 当前转义拼接、双向好友写入或群成员 N+1 查询有哪些一致性与性能改进空间？
