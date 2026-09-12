/*
 * ============================================================
 * session.h — 订单簿会话
 * ============================================================
 *
 * 一个 Session 代表一次"实验"或"模拟"：
 *   - 有自己独立的订单簿 (LimitOrderBook)
 *   - 有自己的撮合引擎 (MatchingEngine)
 *   - 记录所有成交 (fills)
 *   - 可以随机播种订单来模拟真实盘口
 *
 * 每个 Session 彼此独立，互不影响。
 * 就像开了多个"平行世界"的交易所。
 *
 * ============================================================
 */

#ifndef ORDERBOOK_SESSION_H
#define ORDERBOOK_SESSION_H

#include <string>
#include <vector>
#include <mutex>      // std::mutex —— 每个 Session 一把锁，见下方 mu_ 的注释
#include <optional>   // std::optional（本文件直接用到，不指望传递包含）
#include <cstdint>    // std::uint32_t
#include "orderbook/types.h"
#include "orderbook/limit_order_book.h"
#include "orderbook/matching_engine.h"
#include "orderbook/statistics.h"
#include "orderbook/market_impact.h"

namespace orderbook {

class Session {
public:
    /// 创建一个新的会话
    ///
    /// 参数：
    ///   session_id — 会话唯一标识
    ///   symbol     — 模拟的股票代码（如 "AAPL"）
    Session(const std::string& session_id, const std::string& symbol);

    // ── 查询 ──

    const std::string& session_id() const;
    const std::string& symbol() const;

    /// 获取盘口深度
    DepthSnapshot get_depth(int levels = 10) const;

    /// 获取所有成交记录
    const std::vector<Fill>& get_fills() const;

    /// 获取最近 N 笔成交
    std::vector<Fill> get_recent_fills(int limit = 50) const;

    /// 获取盘口统计
    BookStats get_stats(int depth_levels = 10) const;

    /// 估算市场冲击
    double estimate_impact(int quantity, double volatility, double daily_volume) const;

    /// 查找订单
    std::optional<BookOrder> find_order(const std::string& order_id) const;

    // ── 操作 ──

    /// 提交一个订单
    MatchResult submit_order(BookOrder order);

    /// 撤销一个订单
    bool cancel_order(const std::string& order_id);

    /// 随机播种限价单，模拟一个有流动性的盘口
    ///
    /// 参数：
    ///   count      — 生成多少个订单
    ///   mid_price  — 中间价（买卖盘围绕这个价格生成）
    ///   tick_size  — 最小价格变动单位（如 0.01）
    ///   spread_ticks — 价差有多少个 tick（如 5 表示买卖价差 = 5 × tick_size）
    ///   depth_ticks  — 盘口深度有多少个 tick（向两侧延伸多远）
    ///   min_qty    — 最小订单量
    ///   max_qty    — 最大订单量
    ///
    ///   seed       — 随机数种子。给定则输出完全可复现；
    ///                std::nullopt（默认）走 std::random_device，每次不同。
    ///
    /// ⚠️ 为什么需要可注入的种子：
    /// 原来这里固定用 std::random_device 播种，于是播种结果不可复现。
    /// 那意味着任何以播种盘口为前提的测试都是不可复现的——差分测试报出
    /// 「第 137 步不一致」时无法重放，benchmark 也无法在同一副盘口上复测。
    /// 「打印种子即可复现」是随机化测试的立身之本，所以种子必须能从外部指定。
    ///
    /// 返回：实际生成了多少个订单
    int seed_orders(
        int count,
        Price mid_price,
        TickSize tick_size = TickSize{},
        int spread_ticks = 2,
        int depth_ticks = 20,
        int min_qty = 100,
        int max_qty = 1000,
        std::optional<std::uint32_t> seed = std::nullopt
    );

private:
    /*
     * ── 每个 Session 一把锁 ──
     *
     * REST 服务器（cpp-httplib）默认开 max(8, hardware_concurrency-1) 个工作线程，
     * 每个 handler 都跑在池线程上。同一个 session_id 的两个并发请求会拿到**同一个**
     * Session 对象，而下面这些成员全都是可变共享状态。
     *
     * 不加锁的后果不是「读到旧值」这种良性竞争，而是内存不安全：
     * 一个线程在 PriceLevel::match 里 pop_front()、或在 cleanup() 里 bids_.erase()，
     * 另一个线程正在 cancel_order 里迭代 bids_ —— 迭代器失效 + use-after-free。
     * all_fills_ 的 push_back 扩容与 get_recent_fills 的区间读取同理。
     *
     * 为什么是「每 session 一把」而不是一把全局锁：
     * 不同 session 之间本来就完全隔离（各有自己的簿与撮合引擎），
     * 用全局锁会把「多个并行模拟实验」这个设计初衷废掉 ——
     * session_manager.h 的头注释明确说了它是为此存在的。
     * 粒度选在 session 上，既消除了竞争，又保留了跨 session 的真并行。
     *
     * mutable：get_depth / get_stats / find_order 等是 const 方法，但加锁需要改锁的状态。
     */
    mutable std::mutex mu_;

    std::string session_id_;
    std::string symbol_;
    LimitOrderBook book_;          // 该会话的订单簿
    MatchingEngine engine_;        // 该会话的撮合引擎
    std::vector<Fill> all_fills_;  // 所有成交记录
};

}  // namespace orderbook

#endif // ORDERBOOK_SESSION_H
