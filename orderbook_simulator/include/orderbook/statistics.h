/*
 * ============================================================
 * statistics.h — 盘口统计
 * ============================================================
 *
 * 从订单簿和成交记录中计算各种统计指标：
 *   - 价差、相对价差
 *   - 买卖盘深度
 *   - 买卖失衡
 *   - VWAP（成交量加权平均价）
 *   - 成交统计
 *
 * ============================================================
 */

#ifndef ORDERBOOK_STATISTICS_H
#define ORDERBOOK_STATISTICS_H

#include <vector>
#include "orderbook/price.h"
#include "orderbook/types.h"
#include "orderbook/limit_order_book.h"

namespace orderbook {

/// 统计结果
struct BookStats {
    /*
     * spread 是两个网格价之差，仍在网格上，所以用精确的 Price。
     * 而 spread_bps / mid_price / vwap 本质上是**比值和加权平均**，
     * 不可能落在网格上，保持 double 是正确的，不是偷懒。
     * 这条边界写在这里备查，以免以后有人把它们一起「整数化」。
     */
    Price spread;               // 买卖价差（精确）
    double spread_bps;          // 相对价差（基点 = 万分之一）
    double mid_price;           // 中间价
    int bid_depth;              // 买盘总深度（前 N 档总量）
    int ask_depth;              // 卖盘总深度
    double imbalance;           // 买卖失衡 = (买量-卖量)/(买量+卖量)，范围 [-1, 1]
    double vwap;                // 成交量加权平均价
    int total_fills;            // 总成交笔数
    int total_volume;           // 总成交量

    nlohmann::json to_json() const {
        return {
            {"spread", spread.to_double()},
            {"spread_bps", spread_bps},
            {"mid_price", mid_price},
            {"bid_depth", bid_depth},
            {"ask_depth", ask_depth},
            {"imbalance", imbalance},
            {"vwap", vwap},
            {"total_fills", total_fills},
            {"total_volume", total_volume}
        };
    }
};


class Statistics {
public:
    /// 计算盘口统计
    ///
    /// 参数：
    ///   book       — 订单簿（读取盘口信息）
    ///   fills      — 成交记录列表（计算 VWAP 等）
    ///   depth_levels — 统计多少档深度（默认 10 档）
    static BookStats calculate(
        const LimitOrderBook& book,
        const std::vector<Fill>& fills,
        int depth_levels = 10
    );
};

}  // namespace orderbook

#endif // ORDERBOOK_STATISTICS_H
