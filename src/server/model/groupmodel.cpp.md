# `groupmodel.cpp` 讲解

## 作用概览

该文件实现群资料、群成员及群聊收件人查询。

## 按学习顺序讲解

- `createGroup(group)`：对名称和描述做 MySQL 转义后插入 `allgroup`，成功时用 `mysql_insert_id` 回填 ID。
- `addGroup(userid,groupid,role)`：转义角色并插入 `groupuser`；联合主键防止重复成员。
- `queryGroups(userid)`：先查该用户所在群，再对每个群查询全部成员并构造 `GroupUser`，最后返回完整嵌套对象。
- `queryGroupUsers(userid,groupid)`：查询指定群中除发送者外的用户 ID，供业务层逐个投递。

## 函数详细说明

### `GroupModel::createGroup(group)`

这个函数创建群资料。参数是引用，因为函数会读取 `group.getName()` 和 `group.getDesc()`，也会在插入成功后用 `group.setId(...)` 回填数据库自增 ID。

实现首先连接数据库，失败直接返回 false。连接成功后，为群名称和描述分别申请 `原长度 * 2 + 1` 的缓冲区，这是 `mysql_real_escape_string` 需要的最大空间。转义后再拼接 `insert into allgroup(groupname, groupdesc)` SQL，避免名称或描述里的单引号、反斜杠破坏 SQL。

插入成功时调用 `mysql_insert_id(mysql.getConnection())` 获取自增群 ID，并写回 `Group` 对象。这个 ID 后续会被业务层用于把创建者加入 `groupuser` 表。需要注意，创建群和加入创建者不是同一个事务，严格场景应合并成事务。

### `GroupModel::addGroup(userid, groupid, role)`

这个函数把某个用户加入群。`userid` 是成员 ID，`groupid` 是群 ID，`role` 是群内角色。创建群时角色是 `creator`，普通入群时通常是 `normal`。

实现同样先连接数据库，再对 `role` 做字符串转义，最后插入 `groupuser` 表。`userid` 和 `groupid` 是整数，`role` 是字符串，所以这里主要保护角色字段。

函数返回 `void`，意味着业务层目前无法区分“加入成功”和“重复加入/群不存在/数据库失败”。如果要让前端提示更准确，应改为 bool 或枚举状态，并配合唯一索引处理重复成员。

### `GroupModel::queryGroups(userid)`

这个函数负责登录时组装用户加入的全部群以及群成员详情。第一段 SQL 查询当前用户所在群，把 `allgroup` 与 `groupuser` 连接起来，得到群 ID、群名和群描述，并逐行构造 `Group` 对象。

第二段是对每个 `Group` 再查一次成员列表：关联 `user` 和 `groupuser`，读取成员 ID、昵称、在线状态和群角色，构造 `GroupUser` 后通过 `group.getUsers().push_back(user)` 加入群对象内部。

返回值是完整的 `vector<Group>`。这非常适合登录时一次性初始化客户端状态，但也存在 N+1 查询：用户加入 N 个群，就要额外查询 N 次成员。优化方案是一次 JOIN 拉平所有群成员，再按 group ID 聚合成嵌套结构。

### `GroupModel::queryGroupUsers(userid, groupid)`

这个函数查询群聊收件人。它会查 `groupuser` 表中指定 `groupid` 的所有用户，并排除发送者 `userid`，返回用户 ID 列表。

业务层拿到列表后才决定每个用户如何投递：本地在线直接发，其他节点走 RabbitMQ，不在线则写离线消息。因此这个函数只负责“查收件人”，不参与 ACK、去重或离线兜底。

需要注意，当前查询没有显式校验发送者是否属于该群。如果客户端伪造 groupid，业务层应在进入群发前增加成员身份校验，这是一个很自然的面试优化点。

## 面试重点

重要性中等。常见问题：`queryGroups` 的 N+1 问题、创建群和添加 creator 的事务边界、如何校验发送者确实属于群。当前 `groupChat` 依赖查询成员但没有显式权限查询，面试中应如实说明可增加成员校验。
