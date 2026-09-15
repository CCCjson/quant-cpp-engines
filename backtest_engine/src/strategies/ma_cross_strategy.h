/*
 * ma_cross_strategy.h — 均线交叉策略
 *
 * 这是最经典的技术分析策略之一。
 *
 * 原理：
 * 使用两条移动平均线：一条"快线"（短周期），一条"慢线"（长周期）。
 * - 当快线从下方穿越慢线（"金叉"）→ 买入信号
 * - 当快线从上方穿越慢线（"死叉"）→ 卖出信号
 *
 * 为什么有效？
 * 快线反映近期趋势，慢线反映长期趋势。
 * 金叉意味着"近期开始走强"，死叉意味着"近期开始走弱"。
 *
 * 参数：
 * - fast_period：快线周期（默认 5 天）
 * - slow_period：慢线周期（默认 20 天）
 * - position_pct：仓位比例（默认 95% 的资金）
 *
 * 知识点：
 * - override：明确表示这个方法是重写父类的虚函数
 *   如果打错了方法名，编译器会报错，帮你抓 bug
 * - final：阻止子类进一步重写这个方法（可选）
 */

#pragma once

#include <map>
#include <string>

#include "backtest/strategy_base.h"

namespace backtest {

class MACrossStrategy : public IStrategy {
public:
    /*
     * 构造函数
     * 接受策略参数，有默认值。
     *
     * 默认 fast=5, slow=20：这是最常用的参数组合之一。
     * 也可以试 (10, 30)、(20, 60) 等。
     */
    MACrossStrategy(int fast_period = 5, int slow_period = 20, double position_pct = 0.95);

    // ── 实现 IStrategy 接口 ──

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
    int fast_period_;       // 快线周期
    int slow_period_;       // 慢线周期
    double position_pct_;   // 仓位比例


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
        // 上一根 bar 的均线值（用于判断"穿越"）
        double prev_fast_ma = 0.0;
        double prev_slow_ma = 0.0;
    };
    std::map<std::string, SymbolState> state_;
};

}  // namespace backtest
