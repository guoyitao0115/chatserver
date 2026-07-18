# change2.md — 全项目复查问题与修改说明

## 一、复查范围

复查了以下所有文件：
  src/server/chatservice.cpp, chatserver.cpp, main.cpp
  src/server/db/db.cpp
  src/server/model/usermodel.cpp, groupmodel.cpp, friendmoel.cpp, offlinemessagemodel.cpp
  src/server/redis/redis.cpp
  src/client/main.cpp
  include/public.hpp
  include/server/chatservice.hpp, chatserver.hpp, framecodec.hpp, msgdedup.hpp
  include/server/model/*.hpp, include/server/redis/redis.hpp

---

## 二、发现并修复的问题

### 问题A（已修复）：chatservice.cpp 旧版残留导致 conn->send 裸发送

现象：
  之前多次写入操作导致 chatservice.cpp 仍使用旧版 login/reg/oneChat 实现，
  直接调用 conn->send(json_string) 而未经 FrameCodec::encode()，
  客户端 recvFrame() 会把 JSON 数据前4字节当帧头，解析长度错误，整个接收流乱序崩溃。

修复：完整重写 src/server/chatservice.cpp。
  - 新增 sendJsonFrame(conn, js) 辅助函数（第11-14行），内部调用 FrameCodec::encode()。
  - 所有下行发送全部经过 sendJsonFrame()。
  - deliverMsg() 本地推送也改为 FrameCodec::encode(msg)。

验证：grep -n "conn->send" src/server/chatservice.cpp
  只有第14行 sendJsonFrame 内部的 conn->send(FrameCodec::encode(...))，无裸发送。

---

### 问题B（已修复）：chatservice.hpp deliverMsg 声明为 void，实现为 bool

现象：
  include/server/chatservice.hpp 中 deliverMsg 声明为 void，
  但 chatservice.cpp 实现已改为 bool，编译时类型不匹配报错。

修复：include/server/chatservice.hpp 第54行
  修改前：void deliverMsg(int toUserId, const string &msg, string &route);
  修改后：bool deliverMsg(int toUserId, const string &msg, string &route);

---

### 问题C（已记录，建议修复）：db.cpp 字符集设为 gbk，UTF-8 终端中文乱码

文件：src/server/db/db.cpp 第26行
现象：
  mysql_query(_conn, "set names gbk");
  现代系统终端默认 UTF-8，gbk 设置导致中文消息在数据库与终端之间乱码。

建议修改：
  mysql_query(_conn, "set names utf8mb4");
  同时确保建库时 CREATE DATABASE chat CHARACTER SET utf8mb4。

---

### 问题D（已记录，建议修复）：framecodec.hpp 用 static const 定义常量（非最优）

文件：include/server/framecodec.hpp 第35~38行
现象：
  static const int FRAME_HEADER_LEN = 4;
  static const uint32_t FRAME_MAX_PAYLOAD = 4 * 1024 * 1024;
  static 定义在头文件中，每个包含该头的编译单元各有一份副本，不规范。

建议修改：
  constexpr int      FRAME_HEADER_LEN  = 4;
  constexpr uint32_t FRAME_MAX_PAYLOAD = 4u * 1024u * 1024u;

---

### 问题E（已记录，建议修复）：redis.cpp publish 失败判断不完整

文件：src/server/redis/redis.cpp publish() 函数
现象：只检查 reply==nullptr，未检查 reply->type==REDIS_REPLY_ERROR。

建议修改：
  if (nullptr == reply || reply->type == REDIS_REPLY_ERROR)
  {
      if (reply) freeReplyObject(reply);
      return false;
  }

---

### 问题F（已记录，建议修复）：retryThreadHandler 持锁时调用 sendFrame 阻塞

文件：src/client/main.cpp retryThreadHandler()
现象：
  当前实现在持有 g_pendingMutex 的同时调用 sendFrame()（网络 IO）。
  若 send 阻塞（网络缓冲区满），锁被长时间持有，阻塞 readTaskHandler 中的 removePending()。

建议修改（先收集再发送，IO在锁外）：
  static void retryThreadHandler()
  {
      while (true) {
          this_thread::sleep_for(chrono::milliseconds(500));
          vector<pair<string,string>> toSend;  // {msgId, payload}
          vector<string> toErase;
          {
              lock_guard<mutex> lk(g_pendingMutex);
              auto now = chrono::steady_clock::now();
              for (auto &kv : g_pendingMap) {
                  int waitMs = RETRY_BASE_MS * (1 << kv.second.retryCount);
                  auto ms = duration_cast<chrono::milliseconds>(now-kv.second.lastSent).count();
                  if (ms < waitMs) continue;
                  if (kv.second.retryCount >= MAX_RETRY) { toErase.push_back(kv.first); continue; }
                  kv.second.retryCount++; kv.second.lastSent = now;
                  toSend.push_back({kv.first, kv.second.payload});
              }
              for (auto &id : toErase) {
                  cerr << "[retry] " << id << " FAILED, may be lost!" << endl;
                  g_pendingMap.erase(id);
              }
          }  // 锁释放
          for (auto &p : toSend) {  // IO 在锁外
              if (sendFrame(g_clientFd, p.second) == -1)
                  removePending(p.first);
          }
      }
  }

---

### 问题G（已记录）：usermodel.cpp / groupmodel.cpp / friendmoel.cpp SQL 拼接无转义

文件：src/server/model/usermodel.cpp, groupmodel.cpp, friendmoel.cpp
现象：
  均使用 sprintf 拼接 SQL，用户名/密码含单引号时 SQL 语句错误。
  offlinemessagemodel.cpp 已在 CHANGES.md 中修复（使用 mysql_real_escape_string）。
  其余 model 的 insert/query 同样存在此风险，建议统一补充转义处理。

建议：对所有字符串字段调用 mysql_real_escape_string 后再拼入 SQL。

---

## 三、修改文件汇总

已实际修改（代码已写入文件）：
  问题A: src/server/chatservice.cpp    — 完整重写，帧协议统一
  问题B: include/server/chatservice.hpp — deliverMsg 声明改为 bool

建议后续修复（已在本文档记录代码）：
  问题C: src/server/db/db.cpp          — gbk 改 utf8mb4
  问题D: include/server/framecodec.hpp  — static const 改 constexpr
  问题E: src/server/redis/redis.cpp     — publish 增加 REDIS_REPLY_ERROR 检查
  问题F: src/client/main.cpp            — retryThreadHandler 先收集再IO
  问题G: src/server/model/*.cpp         — SQL 字符串字段补充 mysql_real_escape_string


---

## 四、补充（本轮已实际落地的修复）

在继续流程中，以下建议项已完成实际代码修改：

1. `src/server/db/db.cpp`
   - `set names gbk` 已改为 `set names utf8mb4`。

2. `include/server/framecodec.hpp`
   - `static const` 常量已改为 `constexpr`：
     - `FRAME_HEADER_LEN`
     - `FRAME_MAX_PAYLOAD`

3. `src/server/redis/redis.cpp`
   - `publish()` 增加 `reply->type == REDIS_REPLY_ERROR` 失败分支。

4. `src/client/main.cpp`
   - `retryThreadHandler()` 已改为“锁内只收集、锁外执行 `sendFrame` IO”，避免持锁阻塞。

5. `src/server/chatservice.cpp`
   - 仅保留 `sendJsonFrame()` 内部 `conn->send(FrameCodec::encode(...))`。
   - 其他业务路径不再存在裸 `conn->send(json)`。
