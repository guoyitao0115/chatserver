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
