# `user.hpp` 讲解

## 作用概览

**用户领域对象。** 承载数据库用户行在业务层中的 id、名称、密码摘要和在线状态。它是简单值对象，不负责校验密码或执行 SQL。

阅读位置：`include/server/model/user.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-26 行

```cpp
#ifndef USER_H
#define USER_H

#include <string>
using namespace std;

// User 表的轻量数据对象，承接数据库查询结果与业务层之间的数据传递。
// 这个类不负责数据库访问，只保存用户 id、昵称、密码哈希和在线状态。
class User
{
public:
    // 默认 id=-1 表示“未命中/无效用户”，这让 query 失败时可以返回一个空对象，
    // 上层通过 getId() 判断是否查询成功。
    User(int id = -1, string name = "", string pwd = "", string state = "offline")
    {
        this->id = id;
        this->name = name;
        this->password = pwd;
        this->state = state;
    }

    // setter 主要给 Model 层在 insert/query 后回填数据库生成的字段。
    void setId(int id) { this->id = id; }
    void setName(string name) { this->name = name; }
    void setPwd(string pwd) { this->password = pwd; }
    void setState(string state) { this->state = state; }
```

`User` 是在数据库模型与业务层之间传递的值对象。setter 在查询后逐字段组装对象，getter 在登录响应序列化时读取；对象本身不执行 SQL，也不判断登录或群权限，从而保持职责单一。

### 片段 2：第 27-44 行

```cpp

    // getter 返回值而不是引用，避免调用方直接修改内部状态；对象很小，拷贝成本可接受。
    int getId() { return this->id; }
    string getName() { return this->name; }
    string getPwd() { return this->password; }
    string getState() { return this->state; }

protected:
    // protected 允许 GroupUser 继承复用用户基础字段。
    int id;
    string name;
    // 这里保存的是密码哈希，不应保存明文密码；注册/登录时由 PasswordHasher 处理。
    string password;
    // 与数据库 User.state 对应，常见值为 "online" / "offline"。
    string state;
};

#endif
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？
