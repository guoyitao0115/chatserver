# `usermodel.hpp` 讲解

## 作用概览

**用户数据访问接口。** 定义注册、按 id 查询、密码迁移、在线状态更新和启动时状态复位，业务层不需要知道具体 SQL。

阅读位置：`include/server/model/usermodel.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-25 行

```cpp
#ifndef USERMODEL_H
#define USERMODEL_H

#include "user.hpp"

// UserModel 封装 user 表访问，负责注册、登录查询、密码升级和在线状态维护。
class UserModel {
public:
    // 注册用户。成功后会把数据库自增 id 回填到 user 对象，失败返回 false。
    bool insert(User &user);

    // 根据用户 id 查询用户信息；未找到时返回默认 User(id=-1)。
    User query(int id);

    // 更新用户密码哈希。用于从旧密码格式升级到 PBKDF2 等更安全的存储格式。
    bool updatePassword(int id, const string &pwdHash);

    // 更新用户在线状态。数据库状态主要用于查询展示，真正的跨节点在线路由由 Redis 租约维护。
    bool updateState(User user);

    // 服务启动时批量重置为 offline，清理上次异常退出残留的在线状态。
    void resetState();
};

#endif
```

`User` 是在数据库模型与业务层之间传递的值对象。setter 在查询后逐字段组装对象，getter 在登录响应序列化时读取；对象本身不执行 SQL，也不判断登录或群权限，从而保持职责单一。

## 面试重点

- 领域对象与数据访问层如何分工，业务层为什么不直接拼 SQL？

- 当前转义拼接、双向好友写入或群成员 N+1 查询有哪些一致性与性能改进空间？
