#include "usermodel.hpp"
#include "db.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
using namespace std;

/**
 * @brief 创建用户，并回填数据库生成的用户 ID。
 * @param user 包含用户名、bcrypt 密码哈希和初始状态的领域对象。
 * @return INSERT 成功时返回 true，否则返回 false。
 *
 * 所有字符串字段均在已建立的 MySQL 连接上转义，既防止引号破坏
 * SQL 结构，也保证 bcrypt 中的特殊字符按原值保存。
 */
bool UserModel::insert(User &user)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }

    // 对字符串字段做转义，防止 SQL 注入/语法破坏
    vector<char> escName(user.getName().size() * 2 + 1);
    vector<char> escPwd(user.getPwd().size() * 2 + 1);
    vector<char> escState(user.getState().size() * 2 + 1);

    mysql_real_escape_string(mysql.getConnection(), escName.data(), user.getName().c_str(), (unsigned long)user.getName().size());
    mysql_real_escape_string(mysql.getConnection(), escPwd.data(), user.getPwd().c_str(), (unsigned long)user.getPwd().size());
    mysql_real_escape_string(mysql.getConnection(), escState.data(), user.getState().c_str(), (unsigned long)user.getState().size());

    string sql = "insert into user(name, password, state) values('";
    sql += escName.data();
    sql += "', '";
    sql += escPwd.data();
    sql += "', '";
    sql += escState.data();
    sql += "')";

    if (mysql.update(sql))
    {
        // 获取插入成功的用户数据生成的主键id
        user.setId(mysql_insert_id(mysql.getConnection()));
        return true;
    }

    return false;
}

/**
 * @brief 按主键查询用户的认证与在线状态。
 * @return 命中时返回填充完整字段的 User；不存在或数据库失败时返回默认 User。
 *
 * 返回前先释放 MYSQL_RES，避免早退分支泄漏结果集。调用方通常以
 * user.getId() 是否等于查询 ID 来判断是否命中。
 */
User UserModel::query(int id)
{
    // 1.组装sql语句
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "select * from user where id = %d", id);

    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row != nullptr)
            {
                User user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setPwd(row[2]);
                user.setState(row[3]);
                mysql_free_result(res);
                return user;
            }
            mysql_free_result(res);
        }
    }

    return User();
}

/**
 * @brief 更新用户密码字段，用于历史明文密码的平滑 bcrypt 迁移。
 *
 * 登录时若旧密码校验成功，ChatService 生成新哈希后调用本函数。
 * 哈希仍先转义再写入，函数返回值用于反映数据库是否成功接受升级。
 */
bool UserModel::updatePassword(int id, const string &pwdHash)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }

    vector<char> escPwd(pwdHash.size() * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(),
                             escPwd.data(),
                             pwdHash.c_str(),
                             (unsigned long)pwdHash.size());

    string sql = "update user set password = '";
    sql += escPwd.data();
    sql += "' where id = " + to_string(id);

    return mysql.update(sql);
}

/**
 * @brief 把 User 中的 online/offline 状态写回数据库。
 *
 * 状态字符串经转义后与整数 ID 拼接。调用者在登录失败时会根据返回值
 * 回滚 Redis 路由声明，避免路由已占用而数据库仍是离线的不一致。
 */
bool UserModel::updateState(User user)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }

    vector<char> escState(user.getState().size() * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(),
                             escState.data(),
                             user.getState().c_str(),
                             (unsigned long)user.getState().size());

    string sql = "update user set state = '";
    sql += escState.data();
    sql += "' where id = " + to_string(user.getId());

    return mysql.update(sql);
}

/**
 * @brief 把数据库中所有 online 记录重置为 offline。
 *
 * 用于单节点服务启动/停止后清理残留状态。多节点场景下这是粗粒度
 * 操作，不能代替 Redis 中带服务器归属和 TTL 的实时路由。
 */
void UserModel::resetState()
{
    // 1.组装sql语句
    char sql[1024] = "update user set state = 'offline' where state = 'online'";

    MySQL mysql;
    if (mysql.connect())
    {
        mysql.update(sql);
    }
}
