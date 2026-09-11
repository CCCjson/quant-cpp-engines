/*
 * kdj_strategy.cpp — KDJ 金叉/死叉策略实现
 */

#include "kdj_strategy.h"

namespace backtest {

KDJStrategy::KDJStrategy(int n, int m1, int m2,
                         double oversold, double overbought,
                         double position_pct)
    : n_(n), m1_(m1), m2_(m2)
    , oversold_(oversold)
    , overbought_(overbought)
    , position_pct_(position_pct)
{
}

std::string KDJStrategy::name() const {
    return "KDJ";
}

std::string KDJStrategy::description() const {
    return "KDJ策略：K线在低位上穿D线买入，K线在高位下穿D线卖出";
}

std::map<std::string, std::string> KDJStrategy::param_schema() const {
    return {
        {"n", "KDJ周期, 默认9"},
        {"m1", "K平滑因子, 默认3"},
        {"m2", "D平滑因子, 默认3"},
        {"oversold", "超卖区域, 默认20"},
        {"overbought", "超买区域, 默认80"},
        {"position_pct", "仓位比例, 默认0.95"}
    };
}

std::vector<Order> KDJStrategy::on_bar(const StrategyContext& ctx) {
    std::vector<Order> orders;

    if (ctx.bar_index < n_) {
        return orders;
    }

    auto result = ctx.kdj(n_, m1_, m2_);

    /*
     * ⚠️ 首次评估必须先播种 prev_*，不能直接判交叉。
     *
     * prev_k_ / prev_d_ 的初值都是 50.0，于是在第一个被评估的 bar 上
     * `prev_k_ <= prev_d_` 与 `prev_k_ >= prev_d_` **同时为真** ——
     * 那是一个针对虚构前值的假交叉。上面的早退分支（bar_index < n_）又不更新
     * prev_*，所以这个假交叉一定会发生在第 n_ 根 bar 上。
     *
     * MACDStrategy 本来就有这个守卫（macd_strategy.cpp:44-49），KDJ 漏了。
     */
    if (!initialized_) {
        prev_k_ = result.k;
        prev_d_ = result.d;
        initialized_ = true;
        return orders;
    }

    /*
     * ⚠️ 这两行原来把 oversold_ / overbought_ 用反了：
     *     golden 判的是 `result.k < overbought_`（k < 80，即「不超买」）
     *     death  判的是 `result.k > oversold_`  （k > 20，即「不超卖」）
     *
     * 而 K 绝大部分时间就落在 (20, 80) 区间内，所以两个过滤器几乎恒真，
     * 策略退化成**无区域过滤的裸 K/D 交叉**；`oversold` / `overbought`
     * 两个参数同时是「含义反的」和「近乎失效的」——调参的人会得到与文档
     * 相反的效果。
     *
     * 正确语义（与 description()、param_schema()、README 以及
     * benchmarks/python_reference 的实现全都一致）是：
     *     低位（超卖区）金叉才买，高位（超买区）死叉才卖。
     */
    // K 上穿 D，且在低位区域（超卖区）
    bool golden_cross = (prev_k_ <= prev_d_) && (result.k > result.d) && (result.k < oversold_);
    // K 下穿 D，且在高位区域（超买区）
    bool death_cross = (prev_k_ >= prev_d_) && (result.k < result.d) && (result.k > overbought_);

    if (golden_cross && !ctx.has_position()) {
        double available = ctx.cash * position_pct_;
        // 一手股数由引擎按市场给（A股 100 / crypto 1）——⛔ 别再手写 100
        int qty = ctx.lot_floor(available / ctx.current_bar.close);
        if (qty > 0) {
            orders.push_back(Order::market_buy(ctx.symbol, qty));
        }
    }
    else if (death_cross && ctx.has_position()) {
        orders.push_back(Order::market_sell(ctx.symbol, ctx.position_quantity));
    }

    prev_k_ = result.k;
    prev_d_ = result.d;

    return orders;
}

}  // namespace backtest
