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
