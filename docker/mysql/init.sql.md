# `init.sql` 讲解

## 作用概览

**数据库初始化脚本。** 建立用户、好友、群组、群成员和离线消息表及必要索引。表之间的字段对应 `src/server/model` 中的 SQL，离线表的自增 id 还承担恢复消息时的稳定排序依据。

阅读位置：`docker/mysql/init.sql`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-25 行

```sql
-- 所有聊天文本使用 utf8mb4，保证中文和四字节 Unicode 字符可完整保存。
CREATE DATABASE IF NOT EXISTS chat CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE chat;

-- 用户表只保存账户资料和展示用在线状态；实时节点归属以 Redis 租约为准。
CREATE TABLE IF NOT EXISTS user (
    -- 无符号自增 ID 同时作为客户端登录账号。
    id INT UNSIGNED NOT NULL AUTO_INCREMENT,
    name VARCHAR(50) NOT NULL,
    -- 100 字节可容纳 bcrypt 字符串；应用层禁止写入新的明文密码。
    password VARCHAR(100) NOT NULL,
    state ENUM('online', 'offline') NOT NULL DEFAULT 'offline',
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 好友关系按有向边保存；应用查询会连接 friendid 对应的用户资料。
CREATE TABLE IF NOT EXISTS friend (
    userid INT UNSIGNED NOT NULL,
    friendid INT UNSIGNED NOT NULL,
    -- 联合主键同时防止同一关系被重复插入。
    PRIMARY KEY (userid, friendid),
    -- 删除账户时级联清理其发出和指向它的好友关系。
    CONSTRAINT fk_friend_user FOREIGN KEY (userid) REFERENCES user(id) ON DELETE CASCADE,
    CONSTRAINT fk_friend_target FOREIGN KEY (friendid) REFERENCES user(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

这段 DDL/DML 把运行前必须存在的数据库结构一次性建立起来。字段类型和索引围绕实际查询设计：用户 id 用于关系连接，离线消息按用户过滤并按自增 id 排序。

### 片段 2：第 26-55 行

```sql

-- 群基本信息与成员表分离，避免每个成员重复保存群名称和描述。
CREATE TABLE IF NOT EXISTS allgroup (
    id INT UNSIGNED NOT NULL AUTO_INCREMENT,
    groupname VARCHAR(50) NOT NULL,
    groupdesc VARCHAR(200) NOT NULL DEFAULT '',
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 群成员关联表；一个用户在同一群中只能出现一次。
CREATE TABLE IF NOT EXISTS groupuser (
    groupid INT UNSIGNED NOT NULL,
    userid INT UNSIGNED NOT NULL,
    -- creator/normal 是当前教学项目的最小角色模型，后续可扩展管理员等权限。
    grouprole ENUM('creator', 'normal') NOT NULL DEFAULT 'normal',
    PRIMARY KEY (groupid, userid),
    CONSTRAINT fk_groupuser_group FOREIGN KEY (groupid) REFERENCES allgroup(id) ON DELETE CASCADE,
    CONSTRAINT fk_groupuser_user FOREIGN KEY (userid) REFERENCES user(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 在线投递不可用时保存完整 JSON；登录成功后按自增 ID 顺序取回。
CREATE TABLE IF NOT EXISTS offlinemessage (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    userid INT UNSIGNED NOT NULL,
    -- MEDIUMTEXT 可覆盖协议允许的大正文，但应用层仍限制单帧最大 4 MiB。
    message MEDIUMTEXT NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    -- 组合索引支持按用户过滤并稳定地按插入 ID 读取。
    KEY idx_offline_user (userid, id),
```

这段定义或约束 `allgroup`、`groupuser`、`user`、`offlinemessage`，相关索引/外键为 `fk_groupuser_group`、`fk_groupuser_user`、`idx_offline_user`。约束保证模型层写入的用户、群组和关系 id 有有效来源，索引则服务于按用户查询好友、群和离线消息的实际访问路径。

### 片段 3：第 56-57 行

```sql
    CONSTRAINT fk_offline_user FOREIGN KEY (userid) REFERENCES user(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

这段定义或约束 `user`，相关索引/外键为 `fk_offline_user`。约束保证模型层写入的用户、群组和关系 id 有有效来源，索引则服务于按用户查询好友、群和离线消息的实际访问路径。

## 面试重点

- 当前 SQL 如何避免字符串破坏语句，为什么预处理语句仍是更好的演进方向？

- 离线消息按什么顺序恢复，“读取后删除”在哪些崩溃窗口可能重复或遗漏？
