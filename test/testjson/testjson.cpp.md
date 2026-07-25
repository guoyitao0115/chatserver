# `testjson.cpp` 讲解

## 作用概览

**JSON 学习样例。** 用不同字段类型、容器和嵌套对象演示 nlohmann/json 的序列化与反序列化，为理解项目中“JSON 字符串嵌套在数组里”的登录响应做准备。

阅读位置：`test/testjson/testjson.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-22 行

```cpp
#include "json.hpp"
using json = nlohmann::json;

#include <iostream>
#include <vector>
#include <map>
#include <string>
using namespace std;

// json序列化示例1
string func1()
{
    json js;
    js["msg_type"] = 2;
    js["from"] = "zhang san";
    js["to"] = "li si";
    js["msg"] = "hello, what are you doing now?";

    string sendBuf = js.dump();
    //cout<<sendBuf.c_str()<<endl;
    return sendBuf;
}
```

这些头文件把“JSON 学习样例”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 23-44 行

```cpp

// json序列化示例2
string func2()
{
    json js;
    // 添加数组
    js["id"] = {1, 2, 3, 4, 5};
    // 添加key-value
    js["name"] = "zhang san";
    // 添加对象
    js["msg"]["zhang san"] = "hello world";
    js["msg"]["liu shuo"] = "hello china";
    // 上面等同于下面这句一次性添加数组对象
    js["msg"] = {{"zhang san", "hello world"}, {"liu shuo", "hello china"}};
    //cout << js << endl;
    return js.dump();
}

// json序列化示例代码3
string func3()
{
    json js;
```

这部分完成“JSON 学习样例”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。

### 片段 3：第 45-75 行

```cpp

    // 直接序列化一个vector容器
    vector<int> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(5);

    js["list"] = vec;

    // 直接序列化一个map容器
    map<int, string> m;
    m.insert({1, "挺好的?"});
    m.insert({2, "华山"});
    m.insert({3, "泰山"});

    js["path"] = m;

    string sendBuf = js.dump(); // json数据对象 =》序列化 json字符串
    //cout<<sendBuf<<endl;
    return sendBuf;
}

int main()
{
    string recvBuf = func1();
    // 数据的反序列化   json字符串 =》反序列化 数据对象（看作容器，方便访问）
    json jsbuf = json::parse(recvBuf);
    cout<<jsbuf["msg_type"]<<endl;
    cout<<jsbuf["from"]<<endl;
    cout<<jsbuf["to"]<<endl;
    cout<<jsbuf["msg"]<<endl;
```

登录响应中的好友、群和离线消息数组保存的是嵌套 JSON 字符串。客户端逐项再次 parse，构造本地 User/Group 列表；离线聊天消息不直接打印，而是送入同一顺序缓冲逻辑，保证在线与恢复消息采用一致展示规则。

### 片段 4：第 76-100 行

```cpp

    // cout<<jsbuf["id"]<<endl;
    // auto arr = jsbuf["id"];
    // cout<<arr[2]<<endl;

    // auto msgjs = jsbuf["msg"];
    // cout<<msgjs["zhang san"]<<endl;
    // cout<<msgjs["liu shuo"]<<endl;

    // vector<int> vec = jsbuf["list"]; // js对象里面的数组类型，直接放入vector容器当中
    // for (int &v : vec)
    // {
    //     cout << v << " ";
    // }
    // cout << endl;

    // map<int, string> mymap = jsbuf["path"];
    // for (auto &p : mymap)
    // {
    //     cout << p.first << " " << p.second << endl;
    // }
    // cout << endl;

    return 0;
}
```

这部分完成“JSON 学习样例”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 这段继续落实“JSON 学习样例”的当前分支，并把已确认结果交给紧接着的状态更新；失败路径不会伪装成成功响应。

## 面试重点

- 测试准备了什么外部状态或模拟组件，实际动作经过哪些模块？

- 每个断言证明的是返回值正确，还是“不丢、不重、不乱序、不可冒用”等系统性质？

- 如何避免测试自身的等待竞态和上轮残留状态造成假失败？
