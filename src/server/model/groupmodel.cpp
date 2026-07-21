#include "groupmodel.hpp"
#include "db.h"
#include <vector>
#include <cstdio>

/**
 * @brief 写入群基本信息，并把数据库生成的群 ID 回填到 group。
 * @return 只有 INSERT 成功时返回 true。
 *
 * 群名与描述是外部输入，因此先使用当前连接的 mysql_real_escape_string
 * 处理引号、反斜杠等字符。转义缓冲区按“原长度两倍+1”分配，
 * 满足 MySQL C API 的最大膨胀空间。
 */
bool GroupModel::createGroup(Group &group)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        return false;
    }

    vector<char> escName(group.getName().size() * 2 + 1);
    vector<char> escDesc(group.getDesc().size() * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(), escName.data(), group.getName().c_str(), (unsigned long)group.getName().size());
    mysql_real_escape_string(mysql.getConnection(), escDesc.data(), group.getDesc().c_str(), (unsigned long)group.getDesc().size());

    string sql = "insert into allgroup(groupname, groupdesc) values('";
    sql += escName.data();
    sql += "', '";
    sql += escDesc.data();
    sql += "')";

    if (mysql.update(sql))
    {
        group.setId(mysql_insert_id(mysql.getConnection()));
        return true;
    }

    return false;
}

/**
 * @brief 把用户与群、群角色之间的关系写入 groupuser 表。
 *
 * userid/groupid 是整数，role 则先转义后拼接。创建者传 creator，
 * 普通成员传 normal。当前返回 void，上层无法直接区分“已加入”与“写入失败”。
 */
void GroupModel::addGroup(int userid, int groupid, string role)
{
    MySQL mysql;
    if (mysql.connect())
    {
        vector<char> escRole(role.size() * 2 + 1);
        mysql_real_escape_string(mysql.getConnection(),
                                 escRole.data(),
                                 role.c_str(),
                                 (unsigned long)role.size());

        char sql[1024] = {0};
        snprintf(sql, sizeof(sql),
                 "insert into groupuser values(%d, %d, '%s')",
                 groupid, userid, escRole.data());
        mysql.update(sql);
    }
}

/**
 * @brief 查询用户参加的所有群，并为每个群装载成员与角色。
 * @return Group 列表；每个 Group 内部包含 GroupUser 列表。
 *
 * 执行分两阶段：先联表获取群基本信息，再按 groupid 逐群查询成员。
 * 每个 mysql_use_result 结果都在发起下一条 SQL 前释放，满足流式结果集的资源约束。
 *
 * @note 该实现是经典 N+1 查询；群数较多时可改为一次联表查询后在内存分组。
 */
vector<Group> GroupModel::queryGroups(int userid)
{
    /*
    1. 先根据userid在groupuser表中查询出该用户所属的群组信息
    2. 在根据群组信息，查询属于该群组的所有用户的userid，并且和user表进行多表联合查询，查出用户的详细信息
    */
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql),
             "select a.id,a.groupname,a.groupdesc from allgroup a inner join "
             "groupuser b on a.id = b.groupid where b.userid=%d",
             userid);

    vector<Group> groupVec;

    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            // 查出userid所有的群组信息
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                Group group;
                group.setId(atoi(row[0]));
                group.setName(row[1]);
                group.setDesc(row[2]);
                groupVec.push_back(group);
            }
            mysql_free_result(res);
        }
    }

    // 查询群组的用户信息
    for (Group &group : groupVec)
    {
        snprintf(sql, sizeof(sql),
                 "select a.id,a.name,a.state,b.grouprole from user a "
                 "inner join groupuser b on b.userid = a.id where b.groupid=%d",
                 group.getId());

        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                GroupUser user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                user.setState(row[2]);
                user.setRole(row[3]);
                group.getUsers().push_back(user);
            }
            mysql_free_result(res);
        }
    }
    return groupVec;
}

/**
 * @brief 获取群聊的所有目标成员 ID，并排除发送者自己。
 *
 * ChatService::groupChat() 使用返回列表逐个调用统一投递逻辑，从而为
 * 本节点、其他节点与离线用户选择不同路由。结果集在本函数内完整释放。
 */
vector<int> GroupModel::queryGroupUsers(int userid, int groupid)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql),
             "select userid from groupuser where groupid = %d and userid != %d",
             groupid, userid);

    vector<int> idVec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                idVec.push_back(atoi(row[0]));
            }
            mysql_free_result(res);
        }
    }
    return idVec;
}
