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
    CONSTRAINT fk_offline_user FOREIGN KEY (userid) REFERENCES user(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
