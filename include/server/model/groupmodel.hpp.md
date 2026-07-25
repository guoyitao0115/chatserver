# `groupmodel.hpp` 讲解

## 作用概览

**群组数据访问接口。** 定义建群、入群、查询用户群列表和查询消息接收成员，分别服务于登录数据装载和群聊扇出。

阅读位置：`include/server/model/groupmodel.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-26 行

```cpp
#ifndef GROUPMODEL_H
#define GROUPMODEL_H

#include "group.hpp"
#include <string>
#include <vector>
using namespace std;

// GroupModel 封装 allgroup/groupuser 两张表，是群创建、入群、群聊路由的数据库边界。
class GroupModel
{
public:
    // 创建群组并把生成的 group.id 回填到传入对象，便于业务层随后把创建者加入 groupuser。
    bool createGroup(Group &group);

    // 加入群组。role 由业务层传入，创建者通常为 creator，普通成员为 normal。
    void addGroup(int userid, int groupid, string role);

    // 查询用户所在的群组，并组装每个群中的成员信息，用于登录时返回完整群列表。
    vector<Group> queryGroups(int userid);

    // 查询指定群除发送者外的成员 id。群聊发送时业务层遍历这个列表逐个投递或存离线。
    vector<int> queryGroupUsers(int userid, int groupid);
};

#endif
```

群聊扇出集合不含发送者本人。每个成员独立走统一投递，route 计数用于日志观察本地、MQ 和离线比例；`allDelivered` 累积所有结果，不能因为前几个成员成功就提前给整条群消息成功 ACK。

## 面试重点

- 领域对象与数据访问层如何分工，业务层为什么不直接拼 SQL？

- 当前转义拼接、双向好友写入或群成员 N+1 查询有哪些一致性与性能改进空间？
