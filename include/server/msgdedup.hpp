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
     * maxSize    ：LRU 缓存最大条目数，超出后淘汰最旧条目。
     * ttlSeconds ：条目存活秒数，超时后视为过期（允许相同 ID 重新处理）。
     *
     * 该类型维护 mutex，不能被复制；调用方应在服务生命周期内复用同一个实例，
     * 否则每次新建对象都会丢失之前的去重窗口。maxSize 应大于 0、ttlSeconds 应为
     * 正数，当前实现不单独校验这两个构造参数。
     */
    explicit MsgDedup(size_t maxSize = 10000, time_t ttlSeconds = 120)
        : _maxSize(maxSize), _ttlSeconds(ttlSeconds)
    {
    }

    /*
     * 原子地检查并登记 message_id。
     * 若记录已存在且未过期，则只刷新其 LRU 访问位置；若不存在或已超时，则把它
     * 立即登记为“已处理”。检查与写入处于同一把锁内，多个 I/O 线程同时收到相同
     * ID 时最多只有一个线程得到 false。
     *
     * 返回值：
     *   true  —— 重复消息（TTL内），调用方应返回 ACK_DEDUP 并跳过处理
     *   false —— 新消息（或已超 TTL 的旧 ID），调用方正常处理
     *
     * 注意：false 表示去重标记已经占位，不代表后续投递一定成功。业务处理失败时
     * 必须调用 forget() 撤销标记，否则发送方在 TTL 内重试会被错误地当作已完成。
     * 空 ID 为兼容旧客户端始终返回 false，也不会占用缓存。
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

    /**
     * 投递失败时撤销本地处理标记，使发送端可以用同一 message_id 重试。
     * 不存在或空 ID 属于幂等成功，不会报错。删除 map 前先删除链表节点，以保证
     * Entry 中保存的迭代器不被遗留；整个操作与 isDuplicate() 使用同一把锁。
     */
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
    // map 中的值：保存首次登记时刻和对应链表节点，从而能 O(1) 刷新或删除。
    struct Entry
    {
        time_t                   timestamp; // 首次登记时间；重复访问只刷新 LRU，不延长 TTL
        list<string>::iterator   listIt;    // 指向 _lruList 中同一 message_id 的稳定迭代器
    };

    size_t                           _maxSize;    // 内存上限，以条目数而非字节数计
    time_t                           _ttlSeconds; // 从首次登记开始计算的固定有效期（秒）
    mutex                            _mutex;      // 同时保护链表、哈希表及二者的一致性
    list<string>                     _lruList;    // 访问顺序：头部最新，尾部为容量淘汰对象
    unordered_map<string, Entry>     _map;        // message_id -> 元数据，平均 O(1) 查询
};

#endif // MSGDEDUP_HPP
