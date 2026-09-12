/*
 * ============================================================
 * limit_order_book.h — 限价订单簿（核心数据结构）
 * ============================================================
 *
 * 限价订单簿（Limit Order Book, LOB）是证券交易所的核心。
 * 它维护两棵"价格树"：
 *
 *   卖盘 (asks) — 从低到高排列：
 *     102.00  [200 股]
 *     101.50  [300 股]
 *     101.00  [150 股]  ← best_ask（最低卖价，最优卖价）
 *     ─────── 价差 (spread) ───────
 *     100.50  [100 股]  ← best_bid（最高买价，最优买价）
 *     100.00  [250 股]
 *      99.50  [400 股]
 *   买盘 (bids) — 从高到低排列
 *
 * 为什么买盘要从高到低？
 *   因为出价最高的买家应该排在最前面（最有可能成交）
 *
 * 为什么卖盘要从低到高？
 *   因为要价最低的卖家应该排在最前面（最有可能成交）
 *
 * 数据结构选择：std::map
 *   std::map 内部是红黑树（一种自平衡二叉搜索树），key 自动排序。
 *   - 插入/删除/查找都是 O(log n)
 *   - key 始终有序，可以快速取到最大/最小值
 *   - 比 unordered_map（哈希表）多了排序功能，这正是我们需要的
 *
 * ============================================================
 */

#ifndef ORDERBOOK_LIMIT_ORDER_BOOK_H
#define ORDERBOOK_LIMIT_ORDER_BOOK_H

#include <map>
#include <unordered_map>
#include <optional>    // std::optional — 表示"可能没有值"
#include <vector>
#include <string>
#include <functional>  // std::greater — 用于让 map 降序排列
#include "orderbook/types.h"
#include "orderbook/price_level.h"

namespace orderbook {

/// 一档盘口信息（用于 get_depth 返回值）
struct DepthLevel {
    Price price;
    int quantity;       // 该价位总量
    int order_count;    // 该价位订单数

    nlohmann::json to_json() const {
        return {
            {"price", price.to_double()},
            {"quantity", quantity},
            {"order_count", order_count}
        };
    }
};

/// 盘口快照（买卖双方的深度信息）
struct DepthSnapshot {
    std::vector<DepthLevel> bids;    // 买盘（从高到低）
    std::vector<DepthLevel> asks;    // 卖盘（从低到高）
    /*
     * spread 是两个价格之差，仍在网格上，所以是精确的 Price。
     * mid_price 不是：(bid+ask)/2 在 raw 为奇数和时落在网格之外
     * （例如 bid=100.0001, ask=100.0002），本来就不是一个可挂单的价格。
     * 所以它保持 double，并且这一点是刻意的、写在这里备查。
     */
    Price spread;                     // 买卖价差（精确）
    double mid_price;                 // 中间价（可能落在网格外，故为 double）

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["bids"] = nlohmann::json::array();
        for (const auto& b : bids) j["bids"].push_back(b.to_json());
        j["asks"] = nlohmann::json::array();
        for (const auto& a : asks) j["asks"].push_back(a.to_json());
        j["spread"] = spread.to_double();
        j["mid_price"] = mid_price;
        return j;
    }
};


class LimitOrderBook {
public:
    LimitOrderBook();

    // ============================================================
    // 查询方法（只读）
    // ============================================================

    /// 最优买价（买盘里出价最高的）
    /// 返回 std::optional<double>：
    ///   如果买盘有订单 → 返回最高买价
    ///   如果买盘为空   → 返回 std::nullopt（表示"没有值"）
    /// 为什么用 optional？因为订单簿可能是空的，没有最优价
    std::optional<Price> best_bid() const;

    /// 最优卖价（卖盘里要价最低的）
    std::optional<Price> best_ask() const;

    /// 买卖价差 = best_ask - best_bid
    /// 如果任何一边为空，返回 nullopt
    std::optional<Price> spread() const;

    /// 中间价 = (best_bid + best_ask) / 2
    std::optional<double> mid_price() const;

    /// 获取盘口深度（前 N 档）
    DepthSnapshot get_depth(int levels = 10) const;

    /// 获取买盘某价位上的总量（如果该价位不存在则返回 0）
    int bid_quantity_at(Price price) const;

    /// 获取卖盘某价位上的总量
    int ask_quantity_at(Price price) const;

    /// 通过 order_id 查找订单（可能在买盘或卖盘里）
    std::optional<BookOrder> find_order(const std::string& order_id) const;

    // ============================================================
    // 修改方法
    // ============================================================

    /// 添加一个限价单到订单簿
    /// 买单放到 bids_，卖单放到 asks_
    void add_order(BookOrder order);

    /// 撤销一个订单（需要同时知道是在买盘还是卖盘）
    /// 返回是否成功
    bool cancel_order(const std::string& order_id);

    /// 获取买盘对手盘（给卖单撮合用）— 从最高买价开始
    /// 返回指向内部 PriceLevel 的指针，调用者通过它撮合
    PriceLevel* best_bid_level();

    /// 获取卖盘对手盘（给买单撮合用）— 从最低卖价开始
    PriceLevel* best_ask_level();

    /// 移除空的价位（撮合后某价位可能没有订单了）
    void cleanup();

private:
    // ── 核心数据 ──

    // 买盘：std::map<价格, PriceLevel, 比较器>
    //
    // std::greater<double> 让 map 按 key 从大到小排列：
    //   100.50 → PriceLevel(100.50)   ← begin() 指向这里（最高买价）
    //   100.00 → PriceLevel(100.00)
    //    99.50 → PriceLevel(99.50)
    //
    // 默认的 map 是从小到大排的，但买盘需要最高价在前面
    std::map<Price, PriceLevel, std::greater<Price>> bids_;

    // 卖盘：默认升序就行（最低卖价在前面）
    //   101.00 → PriceLevel(101.00)   ← begin() 指向这里（最低卖价）
    //   101.50 → PriceLevel(101.50)
    //   102.00 → PriceLevel(102.00)
    std::map<Price, PriceLevel> asks_;

    /*
     * ── order_id → 所在档位 的索引 ──
     *
     * 撤单原来是全簿线性扫描：遍历买盘每一档、每档再遍历整条 deque，
     * 找不到再遍历卖盘。实测撤单 p50 = 42,209ns 而下单 p50 = 834ns —— 慢 50 倍，
     * 而且撤一个不存在的 id 永远是最坏情况（两边都扫完）。
     *
     * 有了这个索引，撤单变成一次哈希查找 + 该档内的短扫描。
     *
     * ⚠️ 关于陈旧条目的设计取舍（重要）：
     *
     * 撮合是 MatchingEngine 直接拿 best_*_level() 返回的指针调
     * PriceLevel::match 完成的，LimitOrderBook 看不到。所以成交离场的订单
     * 由 match 的 retired_ids 输出参数报出、再由调用方交给 retire_orders()
     * 清理（matching_engine 已这么做）。
     *
     * 但**正确性不依赖这条清理链**：万一将来有新代码路径忘了调
     * retire_orders()，索引里会留下陈旧条目，此时 cancel_order 会按索引找到
     * 那个档、调 remove_order 找不到活跃单、返回 false —— 而 false 正是
     * 「撤一个已成交订单」的正确答案。忘记清理只会让索引变大，不会让行为出错。
     * 这个性质是刻意设计的：让容易忘的事只影响内存，不影响语义。
     */
    std::unordered_map<std::string, std::pair<Side, Price>> index_;

public:
    /// 把这些 order_id 从索引里移除（它们已因全部成交而离开簿）
    ///
    /// 由撮合方在调用 PriceLevel::match 之后传入 retired_ids 调用。
    /// 见上面 index_ 的注释：漏调只会让索引变大，不会导致撤单行为出错。
    void retire_orders(const std::vector<std::string>& ids);
};

}  // namespace orderbook

#endif // ORDERBOOK_LIMIT_ORDER_BOOK_H
