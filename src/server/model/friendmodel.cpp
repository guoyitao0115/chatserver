#include "friendmodel.hpp"
#include "db.h"
#include <cstdio>
#include <cstdlib>

/**
 * @brief 在 friend 关系表中记录“userid 关注 friendid”。
 *
 * 两个参数均是整数，通过 snprintf 写入固定长度缓冲区，不会把用户
 * 提供的字符串直接拼入 SQL。连接或写入失败由 MySQL 封装记录错误。
 *
 * @note 当前表达的是单向关系；若业务要求双向好友，应由事务同时写入
 * 两条关系，并依赖唯一索引防止重复。
 */
void FriendModel::insert(int userid, int friendid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "insert into friend values(%d, %d)", userid, friendid);

    MySQL mysql;
    if (mysql.connect())
    {
        mysql.update(sql);
    }
}

/**
 * @brief 查询指定用户的好友基本信息。
 * @return 按数据库返回顺序组装的 User 列表；连接或查询失败时返回空列表。
 *
 * SQL 先从 friend 表找到 friendid，再与 user 表连接取出 id/name/state。
 * MYSQL_RES 由本函数拥有，遍历完成后显式释放，而连接由 MySQL 析构函数回收。
 */
vector<User> FriendModel::query(int userid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql),
             "select a.id,a.name,a.state from user a "
             "inner join friend b on b.friendid = a.id where b.userid=%d",
             userid);

    vector<User> vec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                User user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setState(row[2]);
                vec.push_back(user);
            }
            mysql_free_result(res);
        }
    }
    return vec;
}
