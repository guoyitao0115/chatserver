# `friendmodel.cpp` 讲解

## 作用概览

该实现把好友关系写入 MySQL，并查询好友的用户资料。

## 按学习顺序讲解

- `FriendModel::insert(userid,friendid)`：建立数据库连接并插入 `friend` 表。联合主键会拒绝重复关系。
- `FriendModel::query(userid)`：使用 JOIN 找到好友对应的 `user` 行，逐行构造 `User`，释放结果集后返回。

## 函数详细说明

### `FriendModel::insert(userid, friendid)`

这个函数把好友关系写入 `friend` 表。`userid` 是当前用户，`friendid` 是被添加的好友。调用入口通常是 `ChatService::addFriend`，业务层完成登录态校验后才会调用它。

实现上先用 `snprintf` 组装 `insert into friend values(%d, %d)`。两个参数都是整数 ID，不涉及用户输入字符串，所以 SQL 注入风险相对低；但仍建议生产环境使用预编译语句统一风格。

随后创建局部 `MySQL` 对象并连接数据库，连接成功后执行 `mysql.update(sql)`。当前函数返回 `void`，所以插入失败只会在 MySQL 封装里记录日志，业务层无法知道是重复好友、数据库故障还是外键问题。若要提升体验，应把返回值改成状态码。

### `FriendModel::query(userid)`

这个函数查询用户好友列表。它先组装 JOIN SQL：从 `friend` 表找到当前用户的 `friendid`，再关联 `user` 表读取好友的 `id/name/state`。

执行流程是：创建结果 vector，连接数据库，执行查询，循环 `mysql_fetch_row` 读取每一行。每行都会创建一个 `User` 对象，只填充好友展示需要的字段，然后 push 到返回列表中。

读取结束后调用 `mysql_free_result(res)` 释放结果集，这是使用 `mysql_use_result` 时必须做的动作。返回值是好友对象列表；如果数据库连接失败、查询失败或用户没有好友，都会返回空 vector，业务层不会区分这些原因。

## 面试重点

重要性中等偏低。可能问题：插入失败为何没有返回值？当前接口无法区分重复与数据库故障，可改为状态返回；是否存在 SQL 注入？两个参数是已转成整数的 ID，风险较低。
