# `db.cpp` 讲解

## 作用概览

这是 MySQL 最小封装的实现，从环境读取连接配置，并为各 Model 提供通用更新、查询和转义所需连接句柄。

## 按学习顺序讲解

- `MySQL::MySQL()`：调用 `mysql_init(nullptr)` 初始化句柄。
- `MySQL::~MySQL()`：非空时调用 `mysql_close`。
- `connect()`：读取 `CHAT_MYSQL_*`，调用 `mysql_real_connect`，成功后设置连接字符集为 `utf8mb4`，失败记录 Muduo 日志。
- `update(sql)`：执行 SQL；失败记录错误并返回 false。
- `query(sql)`：执行 SQL 后调用 `mysql_use_result` 返回结果集；失败返回空指针。
- `getConnection()`：让 Model 使用原始连接执行 `mysql_real_escape_string`。

## 函数详细说明

### `MySQL::MySQL()`

构造函数调用 `mysql_init(nullptr)` 初始化 MySQL C API 连接句柄，并保存到 `_conn`。这一步只是在本地准备连接对象，还没有真正连到数据库。

如果初始化失败，`_conn` 可能为空。后续 `connect()` 调用依赖这个句柄，因此工程化版本可以在构造后检查空指针并把错误向上返回。当前项目保持简单封装，由后续连接失败日志体现问题。

### `MySQL::~MySQL()`

析构函数负责释放数据库连接。只要 `_conn` 非空，就调用 `mysql_close(_conn)`。因为各个 Model 通常在函数内部创建局部 `MySQL mysql;`，所以函数结束时析构会自动关闭连接。

这个设计好理解，但每次数据库操作都新建连接，性能上不适合高并发。面试时可以说明后续应引入连接池，把“每请求建连”改成“从池中借还连接”。

### `connect()`

该函数读取 `CHAT_MYSQL_HOST`、`CHAT_MYSQL_USER`、`CHAT_MYSQL_PASSWORD`、`CHAT_MYSQL_DATABASE`、`CHAT_MYSQL_PORT` 等环境变量，没有配置时使用默认值。随后调用 `mysql_real_connect` 建立实际 TCP 连接和数据库会话。

连接成功后会设置字符集为 `utf8mb4`，这一步很重要：聊天内容、昵称、群描述可能包含中文或 emoji，如果字符集不一致，可能写入乱码或截断。连接失败时不会打印密码，只记录 host、port、database 和错误原因。

返回值是布尔语义：成功返回非空指针转换后的 true，失败返回 false。调用方必须先判断 `connect()`，再执行 `update/query` 或 `mysql_real_escape_string`。

### `update(sql)`

这个函数执行写操作或不需要结果集的 SQL，例如 insert、update、delete。参数 `sql` 是已经组装好的 SQL 字符串。

内部调用 `mysql_query`，失败时记录 `mysql_error(_conn)` 并返回 false。日志刻意不打印完整 SQL，因为 SQL 中可能包含密码哈希或聊天内容，直接打印会造成敏感信息泄漏。

成功返回 true。调用方通常据此决定是否给客户端返回成功，例如注册、创建群、离线消息落库都依赖这个结果。

### `query(sql)`

这个函数执行查询 SQL，并返回 `MYSQL_RES*` 结果集。内部同样用 `mysql_query` 发送 SQL，如果失败则记录错误并返回 `nullptr`。

查询成功后调用 `mysql_use_result(_conn)`，这是流式读取结果的方式。优点是内存占用较低，缺点是必须尽快把结果读完并 `mysql_free_result`，否则该连接不能继续执行下一条 SQL。当前项目每个查询都在同一函数内读取并释放，符合这个约束。

### `getConnection()`

这个函数把底层 `MYSQL*` 暴露给 Model，主要用途是调用 `mysql_real_escape_string` 和 `mysql_insert_id`。例如注册时需要转义昵称和密码哈希，创建群后需要取自增群 ID。

它让封装变得不那么纯粹，但减少了重新包装 MySQL C API 的代码。面试时可以说明：更规范的做法是继续封装“转义字符串”“获取自增 ID”“预编译语句”等能力，避免上层直接依赖底层连接。

## 面试重点

重要性中等。常见问题：`mysql_use_result` 与 `mysql_store_result` 区别？前者流式、内存低但占用连接直到读完；当前每请求连接的成本如何优化？使用连接池；为何仍推荐预编译语句？比手工转义更安全并可复用执行计划。
