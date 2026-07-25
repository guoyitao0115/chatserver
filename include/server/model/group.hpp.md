# `group.hpp` 讲解

## 作用概览

**群组领域对象。** 把群基本信息和成员列表放在一个对象中，供登录响应一次返回用户已加入的群及成员状态。

阅读位置：`include/server/model/group.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-25 行

```cpp
#ifndef GROUP_H
#define GROUP_H

#include "groupuser.hpp"
#include <string>
#include <vector>
using namespace std;

// Group 表的轻量数据对象，同时聚合群成员列表。
// 单独群信息来自 allgroup，成员信息由 groupuser + user 联表查询后填入 users。
class Group
{
public:
    // id=-1 表示尚未入库或查询失败；createGroup 成功后 Model 会回填数据库自增 id。
    Group(int id = -1, string name = "", string desc = "")
    {
        this->id = id;
        this->name = name;
        this->desc = desc;
    }

    // setter 供 Model 层组装查询结果，业务层通常只读取这些字段。
    void setId(int id) { this->id = id; }
    void setName(string name) { this->name = name; }
    void setDesc(string desc) { this->desc = desc; }
```

`Group` 是在数据库模型与业务层之间传递的值对象。setter 在查询后逐字段组装对象，getter 在登录响应序列化时读取；对象本身不执行 SQL，也不判断登录或群权限，从而保持职责单一。

### 片段 2：第 26-43 行

```cpp

    // getUsers 返回引用是为了让 GroupModel 在查询成员时直接 push_back，
    // 避免每次修改成员列表都复制整个 vector。
    int getId() { return this->id; }
    string getName() { return this->name; }
    string getDesc() { return this->desc; }
    vector<GroupUser> &getUsers() { return this->users; }

private:
    // 对应 allgroup 表的 id/groupname/groupdesc 字段。
    int id;
    string name;
    string desc;
    // 查询用户所在群组时携带成员列表，便于登录响应一次返回群和成员关系。
    vector<GroupUser> users;
};

#endif
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？
