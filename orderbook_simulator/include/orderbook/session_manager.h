/*
 * ============================================================
 * session_manager.h — 多会话管理器
 * ============================================================
 *
 * SessionManager 管理多个 Session（多个并行的模拟实验）。
 *
 * 内部用 std::unordered_map 存储所有会话：
 *   key   = session_id (字符串)
 *   value = shared_ptr<Session>
 *
 * 为什么是 shared_ptr 而不是 unique_ptr？
 *   这里原本用的是 unique_ptr（独占所有权），get_session 从中借出裸指针。
 *   在单线程下没问题，但服务器是多线程的（cpp-httplib 默认 8+ 个工作线程），
 *   于是那个裸指针会悬空：调用方走出临界区之后，另一个线程 remove_session
 *   就把对象销毁了。
 *
 *   换成 shared_ptr 之后，get_session 返回的那份引用本身就是所有权凭证 ——
 *   Session 会活到调用方用完为止。remove_session 只是从表里摘掉它。
 *   详见下方 get_session 与 mu_ 的注释。
 *
 * 为什么用 unordered_map 而不是 map？
 *   - unordered_map 是哈希表，查找 O(1)
 *   - map 是红黑树，查找 O(log n)
 *   - 这里不需要排序，只需要快速按 ID 查找，所以用哈希表更快
 *
 * ============================================================
 */

#ifndef ORDERBOOK_SESSION_MANAGER_H
#define ORDERBOOK_SESSION_MANAGER_H

#include <string>
#include <unordered_map>   // 哈希表
#include <memory>          // std::shared_ptr, std::make_shared
#include <shared_mutex>    // std::shared_mutex —— 读多写少，见下方 mu_ 的注释
#include <vector>
#include "orderbook/session.h"

namespace orderbook {

class SessionManager {
public:
    SessionManager() = default;
    // = default 告诉编译器"用自动生成的默认构造函数就行"

    /// 创建一个新会话
    /// 返回 session_id
    std::string create_session(const std::string& symbol);

    /// 获取指定会话
    /// 如果 session_id 不存在，返回空的 shared_ptr
    ///
    /// ⚠️ 返回 shared_ptr 而不是裸 Session*，这一点是并发安全的关键：
    ///
    /// 原来返回的是 `it->second.get()`（从 unique_ptr 借出的裸指针）。
    /// 光给 SessionManager 加一把锁**并不能**修好这件事 —— 锁只保护查表那一瞬间，
    /// 调用方拿着裸指针走出临界区之后，另一个线程调 remove_session 就会把对象
    /// 销毁掉，于是那个裸指针悬空（use-after-free）。
    ///
    /// 换成 shared_ptr 之后，调用方持有的那份引用让 Session 活到它用完为止；
    /// remove_session 只是把它从表里摘掉，真正的析构发生在最后一个引用消失时。
    /// 这是「返回值即所有权凭证」的标准做法。
    std::shared_ptr<Session> get_session(const std::string& session_id);

    /// 删除一个会话
    bool remove_session(const std::string& session_id);

    /// 获取所有会话的 ID 列表
    std::vector<std::string> list_sessions() const;

    /// 当前有多少个会话
    size_t session_count() const;

private:
    /*
     * ── 护表的读写锁 ──
     *
     * create_session 的插入可能触发 unordered_map **rehash**（整表重建），
     * 与另一个线程正在进行的 find 并发就会破表 —— 那不是读到旧值，是内存损坏。
     *
     * 用 shared_mutex 而不是普通 mutex：查表（get_session / list_sessions /
     * session_count）远多于建表和删表，读锁可以并发持有，写锁独占。
     *
     * mutable：list_sessions / session_count 是 const 方法。
     */
    mutable std::shared_mutex mu_;

    // key = session_id, value = shared_ptr<Session>（所有权见 get_session 的注释）
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

}  // namespace orderbook

#endif // ORDERBOOK_SESSION_MANAGER_H
