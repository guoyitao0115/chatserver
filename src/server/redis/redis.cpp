#include "redis.hpp"
#include "config.hpp"

#include <iostream>

using namespace std;

Redis::Redis() : _context(nullptr) {}

Redis::~Redis()
{
    lock_guard<mutex> lock(_mutex);
    if (_context != nullptr)
    {
        redisFree(_context);
        _context = nullptr;
    }
}

bool Redis::connect()
{
    lock_guard<mutex> lock(_mutex);

    const string host = chatserver::config::envOr("CHAT_REDIS_HOST", "127.0.0.1");
    const int port = chatserver::config::envIntOr("CHAT_REDIS_PORT", 6379);
    struct timeval timeout {1, 500000};

    _context = redisConnectWithTimeout(host.c_str(), port, timeout);
    if (_context == nullptr || _context->err != 0)
    {
        const string reason = _context != nullptr ? _context->errstr : "allocation failed";
        cerr << "[redis] connect failed: " << reason
             << " host=" << host << ":" << port << endl;
        if (_context != nullptr)
        {
            redisFree(_context);
            _context = nullptr;
        }
        return false;
    }

    cout << "[redis] connected. host=" << host << ":" << port << endl;
    return true;
}

int Redis::markMessageIfFirst(const string &key, int ttlSeconds)
{
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || key.empty() || ttlSeconds <= 0)
    {
        return -1;
    }

    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "SET %b 1 EX %d NX", key.data(), key.size(), ttlSeconds));
    if (reply == nullptr)
    {
        cerr << "[redis] dedup command failed" << endl;
        return -1;
    }

    int result = -1;
    if (reply->type == REDIS_REPLY_STATUS && reply->str != nullptr && string(reply->str) == "OK")
    {
        result = 1;
    }
    else if (reply->type == REDIS_REPLY_NIL)
    {
        result = 0;
    }
    freeReplyObject(reply);
    return result;
}

bool Redis::removeMessageMark(const string &key)
{
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || key.empty())
    {
        return false;
    }

    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "DEL %b", key.data(), key.size()));
    if (reply == nullptr)
    {
        return false;
    }
    const bool ok = reply->type == REDIS_REPLY_INTEGER;
    freeReplyObject(reply);
    return ok;
}

int Redis::claimUserServer(int userid, const string &serverId, int ttlSeconds)
{
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0 || serverId.empty() || ttlSeconds <= 0)
    {
        return -1;
    }

    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "SET %b %b EX %d NX", key.data(), key.size(),
        serverId.data(), serverId.size(), ttlSeconds));
    if (reply == nullptr)
    {
        return -1;
    }
    int result = -1;
    if (reply->type == REDIS_REPLY_STATUS && reply->str != nullptr
        && string(reply->str) == "OK")
    {
        result = 1;
    }
    else if (reply->type == REDIS_REPLY_NIL)
    {
        result = 0;
    }
    freeReplyObject(reply);
    return result;
}

int Redis::refreshUserServerIfMatches(int userid, const string &serverId, int ttlSeconds)
{
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0 || serverId.empty() || ttlSeconds <= 0)
    {
        return -1;
    }

    static const char *script =
        "if redis.call('GET', KEYS[1]) == ARGV[1] then "
        "return redis.call('EXPIRE', KEYS[1], ARGV[2]) else return 0 end";
    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "EVAL %s 1 %b %b %d", script,
        key.data(), key.size(), serverId.data(), serverId.size(), ttlSeconds));
    if (reply == nullptr)
    {
        return -1;
    }
    const int result = reply->type == REDIS_REPLY_INTEGER
                     ? static_cast<int>(reply->integer) : -1;
    freeReplyObject(reply);
    return result;
}

string Redis::getUserServer(int userid)
{
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0)
    {
        return "";
    }

    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "GET %b", key.data(), key.size()));
    if (reply == nullptr)
    {
        return "";
    }

    string serverId;
    if (reply->type == REDIS_REPLY_STRING && reply->str != nullptr)
    {
        serverId.assign(reply->str, reply->len);
    }
    freeReplyObject(reply);
    return serverId;
}

int Redis::removeUserServerIfMatches(int userid, const string &serverId)
{
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0 || serverId.empty())
    {
        return -1;
    }

    static const char *script =
        "local current = redis.call('GET', KEYS[1]); "
        "if not current then return 2 end; "
        "if current == ARGV[1] then return redis.call('DEL', KEYS[1]) end; "
        "return 0";
    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "EVAL %s 1 %b %b", script,
        key.data(), key.size(), serverId.data(), serverId.size()));
    if (reply == nullptr)
    {
        return -1;
    }
    const int result = reply->type == REDIS_REPLY_INTEGER
                     ? static_cast<int>(reply->integer) : -1;
    freeReplyObject(reply);
    return result;
}
