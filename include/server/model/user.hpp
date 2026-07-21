#ifndef USER_H
#define USER_H

#include <string>
using namespace std;

// User 表的轻量数据对象，承接数据库查询结果与业务层之间的数据传递。
// 这个类不负责数据库访问，只保存用户 id、昵称、密码哈希和在线状态。
class User
{
public:
    // 默认 id=-1 表示“未命中/无效用户”，这让 query 失败时可以返回一个空对象，
    // 上层通过 getId() 判断是否查询成功。
    User(int id = -1, string name = "", string pwd = "", string state = "offline")
    {
        this->id = id;
        this->name = name;
        this->password = pwd;
        this->state = state;
    }

    // setter 主要给 Model 层在 insert/query 后回填数据库生成的字段。
    void setId(int id) { this->id = id; }
    void setName(string name) { this->name = name; }
    void setPwd(string pwd) { this->password = pwd; }
    void setState(string state) { this->state = state; }

    // getter 返回值而不是引用，避免调用方直接修改内部状态；对象很小，拷贝成本可接受。
    int getId() { return this->id; }
    string getName() { return this->name; }
    string getPwd() { return this->password; }
    string getState() { return this->state; }

protected:
    // protected 允许 GroupUser 继承复用用户基础字段。
    int id;
    string name;
    // 这里保存的是密码哈希，不应保存明文密码；注册/登录时由 PasswordHasher 处理。
    string password;
    // 与数据库 User.state 对应，常见值为 "online" / "offline"。
    string state;
};

#endif
