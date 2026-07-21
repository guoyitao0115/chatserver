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
