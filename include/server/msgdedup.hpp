#ifndef MSGDEDUP_HPP
#define MSGDEDUP_HPP

/*
 * ============================================================
 * 消息去重模块 —— 防止重发导致重复消息
 * ============================================================
 * 原理：
 *   客户端对每条业务消息分配全局唯一 message_id（格式建议：
 *   "<senderid>_<timestamp_ms>_<seqno>"）。
 *   服务端用 LRU + TTL 缓存最近已处理过的 message_id，
 *   收到重复 message_id 时直接返回 ACK_DEDUP，不重复落库/转发。
 *
 * 数据结构：
 *   - unordered_map<string, Entry>：O(1) 查找
 *   - list<string>：LRU 淘汰顺序（最近访问的在链表头）
 *   - 容量上限 _maxSize：超出时淘汰最旧条目
 *   - TTL _ttlSeconds：超时的条目视为过期（允许重新处理）
 *
 * 线程安全：
 *   内置 mutex，多线程安全。
 * ============================================================
 */

#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <ctime>
using namespace std;

class MsgDedup
{
public:
    /*
     * maxSize    ：LRU缓存最大条目数，超出后淘汰最旧条目
     * ttlSeconds ：条目存活秒数，超时后视为过期（允许相同ID重新处理）
     */
    explicit MsgDedup(size_t maxSize = 10000, time_t ttlSeconds = 120)
        : _maxSize(maxSize), _ttlSeconds(ttlSeconds)
    {
    }

    /*
     * 检查 message_id 是否已处理过（已存在且未过期）。
     * 若为新消息，则将其记录为"已处理"。
     *
     * 返回值：
     *   true  —— 重复消息（TTL内），调用方应返回 ACK_DEDUP 并跳过处理
     *   false —— 新消息（或已超TTL的旧ID），调用方正常处理
     */
    bool isDuplicate(const string &msgId)
    {
        if (msgId.empty())
        {
            // 没有携带 message_id 的消息视为新消息，不去重
            return false;
        }

        lock_guard<mutex> lock(_mutex);
        time_t now = time(nullptr);

        auto it = _map.find(msgId);
        if (it != _map.end())
        {
            // 找到记录，检查是否在TTL内
            if (now - it->second.timestamp < _ttlSeconds)
            {
                // TTL内的重复消息：移到LRU链表头部（刷新访问顺序）
                _lruList.splice(_lruList.begin(), _lruList, it->second.listIt);
                return true; // 重复，调用方跳过
            }
            else
            {
                // 已过TTL，视为新消息：从旧记录位置移除，重新插入
                _lruList.erase(it->second.listIt);
                _map.erase(it);
            }
        }

        // 新消息：插入记录
        _lruList.push_front(msgId);
        _map[msgId] = {now, _lruList.begin()};

        // 容量超限：淘汰最旧条目（链表尾部）
        if (_map.size() > _maxSize)
        {
            const string &oldest = _lruList.back();
            _map.erase(oldest);
            _lruList.pop_back();
        }

        return false; // 新消息，正常处理
    }

    // 投递失败时撤销本地处理标记，使发送端使用同一 message_id 重试。
    void forget(const string &msgId)
    {
        if (msgId.empty()) return;
        lock_guard<mutex> lock(_mutex);
        auto it = _map.find(msgId);
        if (it == _map.end()) return;
        _lruList.erase(it->second.listIt);
        _map.erase(it);
    }

private:
    // LRU节点：记录插入时间戳 + 在lruList中的迭代器（用于O(1)移动）
    struct Entry
    {
        time_t                   timestamp; // 首次处理时间
        list<string>::iterator   listIt;    // 在 _lruList 中的位置
    };

    size_t                           _maxSize;    // LRU容量上限
    time_t                           _ttlSeconds; // 条目有效期（秒）
    mutex                            _mutex;      // 线程安全锁
    list<string>                     _lruList;    // LRU顺序链表（头=最近，尾=最旧）
    unordered_map<string, Entry>     _map;        // 快速查找表
};

#endif // MSGDEDUP_HPP
