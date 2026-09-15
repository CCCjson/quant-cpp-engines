/*
 * bollinger_strategy.h — 布林带策略
 *
 * 价格触及下轨反弹 → 买入
 * 价格触及上轨回落 → 卖出
 */

#pragma once

#include <map>
#include <string>

#include "backtest/strategy_base.h"

namespace backtest {

class BollingerStrategy : public IStrategy {
public:
    BollingerStrategy(int period = 20, double num_std = 2.0,
                      double position_pct = 0.95);

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
    int period_;
    double num_std_;
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
        double prev_close = 0.0;
        double prev_lower = 0.0;
        double prev_upper = 0.0;
    };
    std::map<std::string, SymbolState> state_;
};

}  // namespace backtest
