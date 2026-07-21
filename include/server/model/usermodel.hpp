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
