# `friendmodel.hpp` 讲解

## 作用概览

**好友数据访问接口。** 提供关系写入和好友列表查询。好友详情来自 `friend` 与 `user` 的连接查询，而不是在关系表重复保存用户名和状态。

阅读位置：`include/server/model/friendmodel.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-20 行

```cpp
#ifndef FRIENDMODEL_H
#define FRIENDMODEL_H

#include "user.hpp"
#include <vector>
using namespace std;

// FriendModel 封装 friend 表访问，业务层只关心“加好友”和“查好友列表”。
// 当前好友关系采用单向插入；如果要做双向好友，需要业务层或 SQL 额外插入反向记录。
class FriendModel
{
public:
    // 添加好友关系。实现层会使用 INSERT IGNORE，因此重复添加不会破坏幂等性。
    void insert(int userid, int friendid);

    // 返回用户好友列表。查询结果会联表 user，拿到好友 id/name/state 供登录响应展示。
    vector<User> query(int userid);
};

#endif
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

## 面试重点

- 领域对象与数据访问层如何分工，业务层为什么不直接拼 SQL？

- 当前转义拼接、双向好友写入或群成员 N+1 查询有哪些一致性与性能改进空间？
