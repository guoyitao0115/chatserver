# `group.hpp` 讲解

## 作用概览

`Group` 是群组数据对象，保存群 ID、名称、描述和成员列表，用于 Model 查询结果与登录响应之间传递数据。

## 按学习顺序讲解

- `Group(id,name,desc)`：用默认值或给定值初始化三个基本字段。
- `setId/setName/setDesc`：分别修改 ID、名称和描述。
- `getId/getName/getDesc`：分别读取三个字段。
- `getUsers()`：返回成员向量的可变引用，Model 可直接向其中追加 `GroupUser`。

## 函数详细说明

### `Group(id, name, desc)`

构造函数用于创建一个群对象，既可以接收数据库查询出来的真实字段，也可以使用默认参数构造一个空对象。默认值让模型层在声明局部变量、等待 SQL 查询结果回填时更方便。

`id` 对应群表主键，`name` 是群名称，`desc` 是群描述。创建群时，`id` 通常先是 `-1`，等 `GroupModel::createGroup` 插入数据库后再用自增 ID 回填。

### `setId(id)` / `getId()`

`setId` 用于修改群 ID，典型场景是创建群后把数据库生成的 `LAST_INSERT_ID()` 写回对象。`getId` 用于业务层读取群 ID，例如创建群成功后把创建者加入这个群，或客户端展示群列表。

这里的重点不是算法，而是对象生命周期：创建前 ID 未确定，创建成功后 ID 才能作为后续 `GroupUser` 关系的外键使用。

### `setName(name)` / `getName()`

这组函数负责群名称读写。`setName` 常用于组装对象或以后扩展“修改群资料”接口；`getName` 在登录响应、群列表展示、调试日志中会用到。

当前 getter 返回字符串副本，调用简单但会产生一次拷贝。项目规模不大时可以接受；如果追求接口规范，可以改成 `const string& getName() const`。

### `setDesc(desc)` / `getDesc()`

这组函数负责群描述字段读写。群描述不是核心路由字段，但能提高前端展示信息完整度，也能说明 `Group` 对象不只是群 ID，而是用于承载完整群资料。

和群名称一样，返回副本更简单，返回常量引用更节省拷贝。

### `getUsers()`

`getUsers` 返回群成员列表的可变引用，`GroupModel::queryGroups` 查询到每个群的成员后，会直接通过这个引用向 `_users` 里追加 `GroupUser`。

这个设计的优点是代码短，模型层组装嵌套对象很方便；缺点是封装性弱，外部调用者可以任意修改内部 vector。面试时如果被问到，可以说当前为了简洁采用可变引用，后续可以增加 `addUser(const GroupUser&)` 和只读 getter，让对象边界更清楚。

## 面试重点

重要性较低。若被问到，可说明它是简单 DTO/ORM 对象；`getUsers` 暴露可变引用虽然方便，但封装性较弱，可改为 `addUser` 和只读访问器。
