/*
 * macd_strategy.h — MACD 交叉策略
 *
 * MACD（Moving Average Convergence Divergence）是最常用的趋势跟踪指标。
 *
 * 信号：
 * - DIF 上穿 DEA → 买入（金叉）
 * - DIF 下穿 DEA → 卖出（死叉）
 */

#pragma once

#include <map>
#include <string>

#include "backtest/strategy_base.h"

namespace backtest {

class MACDStrategy : public IStrategy {
public:
    MACDStrategy(int fast_period = 12, int slow_period = 26,
                 int signal_period = 9, double position_pct = 0.95);

    std::string name() const override;
    std::string description() const override;
    std::map<std::string, std::string> param_schema() const override;
    std::vector<Order> on_bar(const StrategyContext& ctx) override;

    /*
     * on_init — 每场回测开始前把跨 bar 状态清回初值。
     *
     * ⚠️ 不是可选的收尾工作，是正确性要求。下面那些 prev_ 成员活得比一次
     *    回测长，而同一个策略实例会被复用于：
     *      · 连续两场回测（服务端复用策略对象）
     *      · 组合回测里的**所有标的**（engine.cpp::set_strategy：全场一个实例）
     *    不清的话，上一场/另一个标的的前值会在这一场的第一个评估 bar 上
     *    造出一次假交叉。tests/test_strategy_reset.cpp 钉住这件事。
     */
    void on_init() override;

private:
    int fast_period_;
    int slow_period_;
    int signal_period_;
    double position_pct_;


    /*
     * 跨 bar 状态**按标的分槽**。
     *
     * 组合回测下所有标的共用同一个策略实例（engine.cpp::set_strategy），
     * 把前值放在标量成员里，A 的前值就会被 B 覆盖 —— 两边互相造出假交叉。
     *
     * ⚠️ on_init() 解决不了这一条：它每场回测只调一次，而一场之内多个标的是
     *    **交替**进来的。所以除了「每场清空」之外，还必须按 ctx.symbol 分开存。
     *    tests/test_strategy_reset.cpp 里那两组用例分别钉住这两件事。
     */
    struct SymbolState {
        double prev_dif = 0.0;
        double prev_dea = 0.0;
        bool initialized = false;
    };
    std::map<std::string, SymbolState> state_;
};

}  // namespace backtest
