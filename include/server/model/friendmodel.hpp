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
