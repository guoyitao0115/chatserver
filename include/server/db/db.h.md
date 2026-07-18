# `db.h` 讲解

## 作用概览

该文件声明一个最小 MySQL C API RAII 包装类。每个 Model 创建短连接对象，通过它执行更新或查询并获得原生连接用于转义。

## 按学习顺序讲解

- `MySQL()`：调用 `mysql_init` 创建连接句柄。
- `~MySQL()`：关闭非空句柄，避免资源泄漏。
- `connect()`：从环境变量读取连接参数并建立数据库连接。
- `update(sql)`：执行 INSERT/UPDATE/DELETE，返回是否成功。
- `query(sql)`：执行查询并返回 `MYSQL_RES*`，调用方负责 `mysql_free_result`。
- `getConnection()`：暴露原始连接，供 `mysql_real_escape_string` 等 API 使用。

## 函数详细说明

### `MySQL()` 与 `~MySQL()`

- 构造函数调用 `mysql_init` 获得 C API 句柄，但此时尚未建立网络连接。
- 析构函数只在句柄非空时 `mysql_close`，同时释放连接、未完成结果等底层资源，体现最基本的 RAII。
- 类当前未显式删除拷贝操作；从资源所有权看应补充禁拷贝或安全移动，避免未来误复制导致重复关闭。

### `bool connect()`

- **输入**：无显式参数，实际从 `CHAT_MYSQL_*` 环境变量读取地址、端口、用户、密码和库名。
- **返回**：握手和字符集设置成功返回 true，否则记录数据库错误并返回 false。
- **调用特征**：Model 通常每个方法创建一个 `MySQL` 局部对象并连接，简单但每次业务都会付出握手成本。

### `bool update(string sql)`

- **输入**：完整 SQL 字符串，适用于 INSERT、UPDATE、DELETE。
- **返回**：`mysql_query` 成功返回 true，失败返回 false；不返回受影响行数。
- **错误处理**：使用连接上的错误信息记录日志；调用方应根据 bool 决定 ACK 或回滚业务标记。

### `MYSQL_RES *query(string sql)`

- **输入/返回**：执行 SELECT，成功返回结果集指针，失败返回 nullptr。
- **所有权**：返回的 `MYSQL_RES*` 由调用者负责 `mysql_free_result`，这是阅读 Model 实现时必须检查的资源点。
- **实现语义**：当前使用 `mysql_use_result`，读取完成前该连接不能执行下一条查询。

### `MYSQL *getConnection()`

- **返回**：非拥有的原生连接指针，生命周期受当前 `MySQL` 对象控制。
- **用途**：Model 用它调用 `mysql_real_escape_string` 和 `mysql_insert_id`。
- **边界**：外部不能缓存该指针到 `MySQL` 对象析构之后，也不应自行关闭它。


## 面试重点

重要性中等。可能问题：当前封装的不足是什么？每次业务查询新建连接，缺少连接池、预编译语句、事务和错误类型；SQL 注入如何处理？当前字符串字段显式转义，但更推荐 prepared statement。
