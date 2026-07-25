# `groupuser.hpp` 讲解

## 作用概览

**群成员领域对象。** 在普通用户字段之外增加群角色，使查询群详情时能同时返回成员身份，例如创建者与普通成员。

阅读位置：`include/server/model/groupuser.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-20 行

```cpp
#ifndef GROUPUSER_H
#define GROUPUSER_H

#include "user.hpp"

// 群成员对象在 User 基础上增加 role 字段。
// 继承 User 可以直接复用 id/name/state 等展示信息，role 则来自 groupuser.grouprole。
class GroupUser : public User
{
public:
    // role 常见值为 "creator" 或 "normal"，业务层可据此区分群主和普通成员。
    void setRole(string role) { this->role = role; }
    string getRole() { return this->role; }

private:
    // 只描述用户在某个群中的角色，不代表用户全局权限。
    string role;
};

#endif
```

`Group` 是在数据库模型与业务层之间传递的值对象。setter 在查询后逐字段组装对象，getter 在登录响应序列化时读取；对象本身不执行 SQL，也不判断登录或群权限，从而保持职责单一。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？
