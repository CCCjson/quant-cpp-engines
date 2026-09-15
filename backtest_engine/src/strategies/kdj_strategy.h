/*
 * kdj_strategy.h — KDJ 金叉/死叉策略
 *
 * K 上穿 D（低位区域）→ 买入
 * K 下穿 D（高位区域）→ 卖出
 */

#pragma once

#include <map>
#include <string>

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
    int n_, m1_, m2_;
    double oversold_;
    double overbought_;
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
        double prev_k = 50.0;
        double prev_d = 50.0;
        // 首次评估只播种 prev_*、不判交叉（否则 50.0 的初值会造出一个假交叉）
        bool initialized = false;
    };
    std::map<std::string, SymbolState> state_;

    /*
     * ✅ 这里原先记着一条「已知遗留问题（本次未改）」：跨 bar 状态是标量成员，
     *    组合回测下会串味，修法是挪到 per-symbol。**现在修了**，就是上面这个
     *    按 ctx.symbol 分槽的 state_。当时那条注释还写着「/run 只装单标的策略，
     *    所以这条路径实际还走不到」—— 那句话也不准：COMBO 里套一个 PAIRS 会
     *    让引擎装进两个标的，另一个子策略就会在两个标的之间串味。
     */
};

}  // namespace backtest
