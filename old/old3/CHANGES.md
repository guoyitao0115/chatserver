# ChatServer 可靠性改造变更文档

> 修改日期：2026-03-25
> 基础版本：chatserver-master (muduo + hiredis + MySQL)

## 总览

| 编号 | 改动主题 | 主要文件 |
|------|---------|---------|
| 改动1 | 帧协议（粘包拆包） | framecodec.hpp, chatserver.hpp/cpp, client/main.cpp |
| 改动2 | ACK机制 | public.hpp, chatservice.hpp/cpp, client/main.cpp |
| 改动3 | message_id | chatservice.cpp, client/main.cpp |
| 改动4 | 去重幂等(LRU+TTL) | msgdedup.hpp, chatservice.hpp/cpp |
| 改动5 | 跨节点兜底 | chatservice.hpp/cpp |
| 改动6 | 离线消息增强 | offlinemessagemodel.hpp/cpp |
| 改动7 | 全链路打点 | chatservice.cpp, chatserver.cpp |

---

## 改动1 帧协议：解决TCP粘包/拆包

### 问题
原 chatserver.cpp onMessage 直接 buffer->retrieveAllAsString() 当一条消息：
- 粘包：两条消息合并导致JSON解析错误
- 拆包：半条消息导致JSON异常

### 帧结构
```
[4字节 payload_len 网络字节序][payload JSON字符串]
```

### 新增文件：include/server/framecodec.hpp
- FrameCodec::encode(payload)：发送时在JSON前加4字节长度头
- FrameCodec::decode(conn,buffer,ts)：muduo Buffer循环提取完整帧，每帧回调一次业务层
- 单帧最大payload 4MB，超出关闭连接防内存撑爆

### 修改文件
- include/server/chatserver.hpp：新增 FrameCodec _codec 成员，新增 onFrameMessage()
- src/server/chatserver.cpp：onMessage改调 _codec.decode()；onFrameMessage安全解析JSON
- src/client/main.cpp：新增 sendFrame()/recvFrame()，所有收发替换为帧协议

---

## 改动2 ACK机制：服务端接收确认

### 问题
原版客户端发完消息无法确认服务端是否收到，"发了但不知道到没到"。

### 方案
新增消息类型 MSG_ACK(=11)，服务端收到业务消息后立即回ACK：
  {"msgid":11, "message_id":"<原消息ID>", "ack_state":0}

### 修改文件
- include/public.hpp：新增 MSG_ACK=11；新增 AckState(ACK_OK=0,ACK_DEDUP=1,ACK_FAIL=2)
- include/server/chatservice.hpp：新增 sendAck(conn,msgId,ackState) 私有方法
- src/server/chatservice.cpp：oneChat()/groupChat()投递后调用sendAck；无message_id跳过ACK
- src/client/main.cpp：接收线程新增MSG_ACK分支，ACK_OK/ACK_DEDUP均从g_pendingMap移除

---

## 改动3 message_id：消息唯一标识

### 问题
原消息无唯一标识，ACK无法对应原消息，去重无法实现。

### 方案
格式：<userid>_<timestamp_ms>_<自增序号>   示例："42_1711382400123_7"

### 修改文件：src/client/main.cpp
- 新增 generateMsgId() 函数（userid + 时间戳ms + 自增序号）
- 新增 g_pendingMap<string,string>：发送加入pending，收ACK移除
- chat()/groupchat() 命令处理自动附加 message_id 字段

---

## 改动4 去重幂等：防止重发重复消息

### 问题
客户端超时重试时，服务端已处理过，重发导致重复落库和展示。

### 新增文件：include/server/msgdedup.hpp
- MsgDedup 类，内置mutex线程安全
- isDuplicate(msgId)：TTL内已见过返回true(回ACK_DEDUP)，否则记录并返回false
- LRU容量默认10000，TTL默认120秒，可构造时配置
- 空 message_id 视为新消息不去重（兼容旧客户端）

### 修改文件
- include/server/chatservice.hpp：新增成员 MsgDedup _dedup
- src/server/chatservice.cpp：oneChat()/groupChat()投递前调用_dedup.isDuplicate()；重复则回ACK_DEDUP返回

---

## 改动5 跨节点兜底：Redis Pub/Sub失败降级

### 问题
原版跨节点 _redis.publish() 失败时消息静默丢失。

### 方案：deliverMsg(toUserId, msg, route) 三段路由
  1. 本节点在线   -> conn->send()              route="local"
  2. 其他节点在线 -> redis.publish()            route="redis"
     publish失败  -> 降级落离线库               route="offline_fallback"
  3. 用户离线     -> offlineMsgModel.insert()   route="offline"

### 修改文件
- include/server/chatservice.hpp：新增 deliverMsg() 私有方法声明
- src/server/chatservice.cpp：oneChat()/groupChat()/handleRedisSubscribeMessage()均改为调用deliverMsg()

---

## 改动6 离线消息链路增强

### 问题
1. sprintf拼接SQL，消息含单引号时SQL报错甚至注入
2. insert失败静默忽略，无法感知丢消息
3. insert返回void，调用方无法判断落库是否成功

### 修改文件
- include/server/model/offlinemessagemodel.hpp：insert签名改为 bool insert(int,string)
- src/server/model/offlinemessagemodel.cpp：
    * 使用 mysql_real_escape_string() 对消息内容转义（防单引号破坏SQL）
    * 所有DB操作失败时记录 LOG_ERROR，不静默丢失
    * SQL缓冲区从1024扩至4096字节

---

## 改动7 全链路可观测性打点

### 打点位置清单

| 位置 | 级别 | 关键字段 |
|------|------|---------|
| oneChat() 消息接收 | INFO | message_id,from,to |
| oneChat() 去重命中 | WARN | message_id,from |
| oneChat() 投递完成 | INFO | message_id,from,to,route,status |
| groupChat() 接收 | INFO | message_id,from,groupid |
| groupChat() 去重命中 | WARN | message_id,from |
| groupChat() 投递汇总 | INFO | groupid,members,local,redis,offline |
| deliverMsg() Redis降级 | WARN | userid |
| handleRedisSubscribeMessage | INFO | userid,route |
| login() 尝试/成功/失败 | INFO/WARN | userid |
| login() 离线消息数 | INFO | userid,count |
| loginout() | INFO | userid |
| clientCloseException() | WARN | userid |
| reg() 成功/失败 | INFO/ERROR | name,id |
| onFrameMessage() JSON异常 | ERROR | payload前200字符 |
| OfflineMsgModel insert失败 | ERROR | userid,sql |

---

## 新增/修改文件汇总

**新增（2个）：**
- include/server/framecodec.hpp   # 帧编解码器（改动1）
- include/server/msgdedup.hpp     # 消息去重器（改动4）

**修改（8个）：**
- include/public.hpp                            # 新增MSG_ACK、AckState（改动2）
- include/server/chatserver.hpp                 # 新增FrameCodec成员（改动1）
- include/server/chatservice.hpp                # 新增sendAck/deliverMsg/_dedup（改动2/4/5）
- include/server/model/offlinemessagemodel.hpp  # insert改返回bool（改动6）
- src/server/chatserver.cpp                     # onMessage改帧协议（改动1/7）
- src/server/chatservice.cpp                    # 全面改造（改动2/3/4/5/7）
- src/server/model/offlinemessagemodel.cpp      # 转义SQL+错误日志（改动6）
- src/client/main.cpp                           # 帧协议+message_id+ACK（改动1/2/3）

---

## 构建说明

```bash
cd /Users/guoyitao/project/chatserver-master
bash autobuild.sh
```

或手动：
```bash
mkdir -p build && cd build
cmake .. && make -j4
```

---

## 注意事项

1. **客户端重试定时器**：本次实现了pending_map基础框架，超时重发定时器需
   在独立线程中轮询g_pendingMap实现（可选扩展，本次未集成）。
2. **Redis Streams**：本次保留Pub/Sub+离线库兜底，切换到
   Redis Streams(XADD/XREADGROUP/XACK)是更彻底改造，可替换Redis类实现。
3. **数据库连接池**：当前每次操作新建MySQL连接，高并发下建议引入连接池。
4. **客户端协议升级**：服务端已全量使用帧协议，旧版裸JSON客户端需同步升级。
