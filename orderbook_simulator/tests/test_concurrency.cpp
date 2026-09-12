/*
 * test_concurrency.cpp — 并发安全的压力测试
 *
 * ============================================================
 * 为什么需要它
 * ============================================================
 *
 * REST 服务器（cpp-httplib）默认开 max(8, hardware_concurrency-1) 个工作线程，
 * 每个 handler 都跑在池线程上。而在加锁之前，这个项目 grep
 * `mutex|atomic|lock_guard|shared_mutex` **零命中** —— 也就是说所有共享可变状态
 * 都是裸奔的。
 *
 * 那些竞争不是「读到旧值」这种良性问题，而是内存不安全：
 *
 *   1. SessionManager::sessions_ 是 unordered_map，create_session 的插入可能触发
 *      **rehash**（整表重建），与另一线程的 find 并发即破表。
 *   2. get_session 原来返回从 unique_ptr 借出的**裸指针**。调用方走出临界区后，
 *      另一线程 remove_session 就会销毁对象 → use-after-free。
 *      光加锁修不好这个，必须改所有权模型（现在返回 shared_ptr）。
 *   3. 同一 session 的并发「下单 + 撤单」：一个线程在 PriceLevel::match 里
 *      pop_front()、或 cleanup() 里 bids_.erase()，另一线程正在 cancel_order 里
 *      迭代 bids_ —— 迭代器失效 + use-after-free。
 *   4. generate_id() 用普通的函数内 static 计数器，`counter++` 是读-改-写，
 *      多线程下会**发出重复 order_id**，而整套按 id 查找/撤单的逻辑都假定 id 唯一。
 *
 * ============================================================
 * 这个文件本身不是证明，TSan 才是
 * ============================================================
 *
 * 下面的用例在普通构建下跑通，只能说明「没崩、没死锁、不变量成立」。
 * 数据竞争的特点就是大多数时候不崩 —— 所以真正的验证手段是：
 *
 *     ./build.sh tsan
 *
 * ThreadSanitizer 会在**实际发生**竞争时报告，而不是等它碰巧导致崩溃。
 * 这些用例的作用是给 TSan 提供足够密集的并发访问，让它有东西可查。
 */

#include <gtest/gtest.h>

#include "orderbook/session_manager.h"
#include "orderbook/types.h"
#include "test_price_helpers.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace orderbook;

namespace {

// 线程数取硬件并发度，但至少 4 —— 单核机器上也要有真并发
unsigned threads_to_use() {
    const unsigned hc = std::thread::hardware_concurrency();
    return hc >= 4 ? hc : 4u;
}

}  // namespace

/*
 * ── 竞争 1 & 2：并发建表 / 查表 / 删表 ──
 *
 * 一组线程不停创建 session（触发 rehash），另一组不停查表与删表。
 * 加锁前这会破表或返回悬空指针；现在应当既不崩也不丢。
 */
TEST(ConcurrencyTest, ConcurrentCreateGetRemoveOnSessionManager) {
    SessionManager mgr;
    const unsigned T = threads_to_use();
    const int per_thread = 200;

    std::vector<std::string> seeded;
    for (int i = 0; i < 50; ++i) seeded.push_back(mgr.create_session("SEED"));

    std::atomic<int> created{0}, found{0}, removed{0};
    std::vector<std::thread> ts;

    // 建表线程：插入会触发 unordered_map rehash
    for (unsigned t = 0; t < T / 2 + 1; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < per_thread; ++i) {
                mgr.create_session("NEW");
                created.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 查表 + 删表线程：与上面的 rehash 并发
    for (unsigned t = 0; t < T / 2 + 1; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < per_thread; ++i) {
                const auto& sid = seeded[(i + t * 7) % seeded.size()];
                // 拿到的是 shared_ptr —— 即使另一个线程此刻把它从表里摘掉，
                // 对象也会活到这里用完
                if (auto s = mgr.get_session(sid)) {
                    (void)s->get_depth(5);
                    found.fetch_add(1, std::memory_order_relaxed);
                }
                if (i % 50 == 0) {
                    if (mgr.remove_session(mgr.create_session("TMP"))) {
                        removed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                (void)mgr.session_count();
                (void)mgr.list_sessions();
            }
        });
    }

    for (auto& th : ts) th.join();

    EXPECT_GT(created.load(), 0);
    EXPECT_GT(found.load(), 0);
    EXPECT_GT(removed.load(), 0);
    // 播种的 50 个从未被删，必须还在
    for (const auto& sid : seeded) {
        EXPECT_NE(mgr.get_session(sid), nullptr) << "播种的 session 不该消失: " << sid;
    }
}

/*
 * ── 竞争 3：同一个 session 上并发下单 / 撤单 / 读盘口 ──
 *
 * 这是最危险的一组：撮合会 pop_front 并 erase 价位，而撤单在迭代价位。
 * 加锁前是迭代器失效 + use-after-free。
 */
TEST(ConcurrencyTest, ConcurrentSubmitCancelAndReadOnOneSession) {
    SessionManager mgr;
    const std::string sid = mgr.create_session("TEST");
    auto session = mgr.get_session(sid);
    ASSERT_NE(session, nullptr);
    session->seed_orders(2000, P(100.0), TS(0.01), 2, 50, 100, 1000, 4242u);

    const unsigned T = threads_to_use();
    const int per_thread = 300;
    std::atomic<int> submitted{0}, cancelled{0}, reads{0};
    std::vector<std::thread> ts;

    for (unsigned t = 0; t < T; ++t) {
        ts.emplace_back([&, t] {
            std::vector<std::string> mine;
            for (int i = 0; i < per_thread; ++i) {
                switch (i % 3) {
                    case 0: {   // 下一个远离盘口的挂单（不会立刻成交）
                        BookOrder o;
                        o.side = (t % 2) ? Side::BUY : Side::SELL;
                        o.order_type = OrderType::LIMIT;
                        o.price = P((o.side == Side::BUY) ? 50.0 - (i % 20) * 0.01
                                                           : 150.0 + (i % 20) * 0.01);
                        o.quantity = 100;
                        const auto r = session->submit_order(std::move(o));
                        mine.push_back(r.order_id);
                        submitted.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    case 1: {   // 撤掉自己下过的一单（与别的线程的撮合并发）
                        if (!mine.empty()) {
                            const std::string id = mine.back();
                            mine.pop_back();
                            if (session->cancel_order(id)) {
                                cancelled.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        break;
                    }
                    default: {  // 穿价单：会真的走撮合，pop_front + erase 价位
                        BookOrder o;
                        o.side = (t % 2) ? Side::BUY : Side::SELL;
                        o.order_type = OrderType::IOC;
                        o.price = P((o.side == Side::BUY) ? 101.0 : 99.0);
                        o.quantity = 50;
                        (void)session->submit_order(std::move(o));
                        (void)session->get_depth(10);
                        (void)session->get_stats(10);
                        reads.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        });
    }

    for (auto& th : ts) th.join();

    EXPECT_GT(submitted.load(), 0);
    EXPECT_GT(cancelled.load(), 0);
    EXPECT_GT(reads.load(), 0);

    // 不变量：并发折腾完之后，簿仍然不交叉
    const auto d = session->get_depth(50);
    if (!d.bids.empty() && !d.asks.empty()) {
        EXPECT_LT(d.bids.front().price, d.asks.front().price)
            << "并发操作后订单簿交叉了 —— 说明有状态被并发破坏";
    }
}

/*
 * ── 竞争 4：generate_id 必须发出唯一的 id ──
 *
 * 原来是普通的函数内 static 计数器，`counter++` 是读-改-写：
 * 两个线程可能读到同一个值、各自 +1、写回同一个数 → 重复 id。
 *
 * 重复 id 会直接破坏 find_order / cancel_order / order_id 索引的正确性，
 * 因为它们全都假定 id 唯一。
 */
TEST(ConcurrencyTest, GenerateIdIsUniqueAcrossThreads) {
    const unsigned T = threads_to_use();
    const int per_thread = 2000;

    std::vector<std::vector<std::string>> buckets(T);
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < T; ++t) {
        ts.emplace_back([&, t] {
            buckets[t].reserve(per_thread);
            for (int i = 0; i < per_thread; ++i) buckets[t].push_back(generate_id());
        });
    }
    for (auto& th : ts) th.join();

    std::set<std::string> all;
    std::size_t total = 0;
    for (const auto& b : buckets) {
        total += b.size();
        all.insert(b.begin(), b.end());
    }

    EXPECT_EQ(all.size(), total)
        << "在 " << T << " 个线程各生成 " << per_thread << " 个 id 的情况下出现了重复：\n"
        << "  生成总数 = " << total << "，去重后 = " << all.size() << "\n"
        << "说明计数器不是原子的 —— 重复 id 会直接破坏按 id 查找/撤单的全部逻辑。";
}
