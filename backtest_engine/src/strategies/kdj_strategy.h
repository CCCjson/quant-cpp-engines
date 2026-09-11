/*
 * kdj_strategy.h — KDJ 金叉/死叉策略
 *
 * K 上穿 D（低位区域）→ 买入
 * K 下穿 D（高位区域）→ 卖出
 */

#pragma once

#include "backtest/strategy_base.h"

namespace backtest {

class KDJStrategy : public IStrategy {
public:
    KDJStrategy(int n = 9, int m1 = 3, int m2 = 3,
                double oversold = 20.0, double overbought = 80.0,
                double position_pct = 0.95);

    std::string name() const override;
    std::string description() const override;
    std::map<std::string, std::string> param_schema() const override;
    std::vector<Order> on_bar(const StrategyContext& ctx) override;

private:
    int n_, m1_, m2_;
    double oversold_;
    double overbought_;
    double position_pct_;

    double prev_k_ = 50.0;
    double prev_d_ = 50.0;
    // 首次评估只播种 prev_*、不判交叉（否则 50.0 的初值会造出一个假交叉）
    bool initialized_ = false;

    /*
     * ⚠️ 已知的遗留问题（本次未改）：这三个成员是**跨 bar 状态**，而
     * BacktestEngine 在组合回测下让所有标的**共用同一个策略实例**
     * （见 engine.cpp:37-43 的警告）。多标的跑 KDJ 会串味。
     * 修法是把状态挪到 per-symbol 的地方（engine.cpp 的 SymbolState），
     * 那是一次结构性改动，不适合和这个阈值修复混在一起。
     * 目前 /run 只装单标的策略，/run_portfolio 只装
     * PortfolioSignalStrategy，所以这条路径实际还走不到。
     */
};

}  // namespace backtest
