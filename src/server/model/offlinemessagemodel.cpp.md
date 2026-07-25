# `offlinemessagemodel.cpp` 讲解

## 作用概览

**离线消息数据访问实现。** 把完整 JSON 文本转义后写入数据库，登录时按自增 id 读取以保持同一用户的落库顺序，再在业务层确认已装入登录响应后删除。

阅读位置：`src/server/model/offlinemessagemodel.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-28 行

```cpp
#include "offlinemessagemodel.hpp"
#include "db.h"
#include <muduo/base/Logging.h>
#include <cstring>
using namespace muduo;

// 离线消息模型是投递链路的最后一道可靠性兜底：本地连接不存在、
// 跨节点路由缺失或 RabbitMQ 发布失败时，ChatService 会把原始 JSON 写入此表。

/**
 * @brief 按用户 ID 持久化一条完整聊天消息。
 * @param userid 最终接收者 ID。
 * @param msg 待恢复投递的 JSON 字符串，上线后会原样返回客户端。
 * @return 只有数据库连接和 INSERT 都成功时返回 true。
 *
 * 转义缓冲区按 2*n+1 分配，避免固定长度数组截断长消息。
 * mysql_real_escape_string 使用已连接句柄，会按当前字符集正确处理特殊字符。
 * 失败日志会带 userid，但不记录消息正文，以减少隐私泄漏。
 */
bool OfflineMsgModel::insert(int userid, string msg)
{
    MySQL mysql;
    if (!mysql.connect())
    {
        LOG_ERROR << "[OfflineMsgModel::insert] connect DB failed, userid="
                  << userid << " msg dropped!";
        return false;
    }
```

离线消息保存完整 JSON 字符串而非拆字段，新增协议字段无需修改表结构。正文按连接字符集转义后插入，返回值直接决定 ChatService 能否回复 ACK_OK。

### 片段 2：第 29-52 行

```cpp

    // 对消息内容做转义，防止消息中含有单引号/反斜杠破坏 SQL 结构
    // mysql_real_escape_string 需要已建立的连接句柄
    size_t msgLen = msg.size();
    // 转义后最长为原来2倍+1
    vector<char> escaped(msgLen * 2 + 1);
    mysql_real_escape_string(mysql.getConnection(),
                             escaped.data(),
                             msg.c_str(),
                             (unsigned long)msgLen);

    // 动态组装，避免固定 4KB 缓冲区截断较长消息。
    string sql = "INSERT INTO offlinemessage(userid, message) VALUES(";
    sql += to_string(userid);
    sql += ", '";
    sql += escaped.data();
    sql += "')";

    if (!mysql.update(sql))
    {
        // 落库失败必须记录错误日志，不能静默丢弃
        LOG_ERROR << "[OfflineMsgModel::insert] insert failed, userid=" << userid;
        return false;
    }
```

动态字符串先按当前 MySQL 连接的字符集执行转义，再拼入 SQL。例如名称含单引号时会作为字段内容保存，而不是提前结束字符串字面量；数值 id 则直接以十进制拼接。更大项目宜进一步改为预处理语句。

这部分处在离线恢复链路：在线路由不可用时保存完整消息，用户登录时按落库顺序装入响应。当前“读取后删除”是一次性交付语义；若发送登录响应前后进程崩溃，客户端仍应依靠 message_id 去重。

### 片段 3：第 53-76 行

```cpp
    return true;
}

/**
 * @brief 删除用户当前全部离线消息。
 *
 * 登录响应已组装完离线消息后调用此函数。当前是“先读取、再删除、
 * 再发送”的简化语义：删除失败可能导致下次重复投递，因此客户端仍需去重；
 * 而删除后、响应到达前进程崩溃，则仍存在丢失窗口。
 */
void OfflineMsgModel::remove(int userid)
{
    char sql[256] = {0};
    snprintf(sql, sizeof(sql),
             "DELETE FROM offlinemessage WHERE userid=%d", userid);

    MySQL mysql;
    if (mysql.connect())
    {
        if (!mysql.update(sql))
        {
            // 删除失败只记录警告，不影响业务流程
            LOG_ERROR << "[OfflineMsgModel::remove] delete failed, userid=" << userid;
        }
```

登录响应已经装入离线消息后按 userId 删除。正常二次登录不会重放；但响应发送与删除不在事务/确认协议中，极端崩溃窗口仍需要客户端 message_id 去重。

### 片段 4：第 77-109 行

```cpp
    }
    else
    {
        LOG_ERROR << "[OfflineMsgModel::remove] connect DB failed, userid=" << userid;
    }
}

/**
 * @brief 按入库顺序取出用户的所有离线消息。
 * @return 消息 JSON 字符串列表；查询失败时返回空列表。
 *
 * ORDER BY id ASC 使结果按自增主键顺序恢复，保留单库写入的先后关系。
 * 每行的 message 被复制到 vector 后即可释放 MYSQL_RES。
 */
vector<string> OfflineMsgModel::query(int userid)
{
    char sql[256] = {0};
    snprintf(sql, sizeof(sql),
             "SELECT message FROM offlinemessage WHERE userid = %d ORDER BY id ASC", userid);

    vector<string> vec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            // 把userid用户的所有离线消息放入vec中返回
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                vec.push_back(row[0]);
            }
```

查询按自增 `id` 升序读取完整 JSON，返回顺序就是登录恢复顺序。业务层再依据消息内 `client_seq` 展示；这里不解析 JSON，避免数据访问层耦合协议字段。

查询成功后逐行读取结果集，把 SQL 列转换为领域对象或字符串集合。代码只在结果非空时访问列，并在结束后释放结果集；返回空集合既可能表示没有数据，也可能伴随日志中的查询失败，需要上层结合语义处理。

### 片段 5：第 110-118 行

```cpp
            mysql_free_result(res);
        }
    }
    else
    {
        LOG_ERROR << "[OfflineMsgModel::query] connect DB failed, userid=" << userid;
    }
    return vec;
}
```

结果集在所有行转换完成后立即释放，再返回已经拥有自身字符串/数值副本的容器。这样后续 SQL 可以复用连接，也不会让领域对象持有指向 MYSQL_RES 内部缓冲的悬空指针。

## 面试重点

- 当前 SQL 如何避免字符串破坏语句，为什么预处理语句仍是更好的演进方向？

- 离线消息按什么顺序恢复，“读取后删除”在哪些崩溃窗口可能重复或遗漏？
