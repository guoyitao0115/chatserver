#include "usermodel.hpp"
#include "db.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
using namespace std;

// User表的增加方法
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

// 根据用户号码查询用户信息
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

// 更新用户密码哈希
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

// 更新用户的状态信息
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

// 重置用户的状态信息
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
