/*
 * test_kdj_strategy.cpp — KDJ 策略的区域过滤测试
 *
 * ============================================================
 * 为什么这个文件是必需的
 * ============================================================
 *
 * 在这个文件出现之前，`grep -riE "kdj" tests/` **零命中** —— KDJ 策略完全没有测试。
 * 而它带着一个真实的缺陷发布了很久：
 *
 *     // 原代码
 *     golden_cross = ... && (result.k < overbought_);   // k < 80，即「不超买」
 *     death_cross  = ... && (result.k > oversold_);     // k > 20，即「不超卖」
 *
 * 两个阈值用反了。K 绝大部分时间就在 (20, 80) 区间内，所以两个过滤器几乎恒真，
 * 策略退化成**无区域过滤的裸 K/D 交叉**；`oversold` / `overbought` 两个参数同时是
 * 「含义反的」和「近乎失效的」——调参的人得到的是与文档相反的效果。
 *
 * 没有测试，这个缺陷两个方向都没有保护：既没有东西发现它，修完也没有东西
 * 防止它被改回去。所以修复必须自带测试。
 *
 * ============================================================
 * 构造序列的 K/D 取值是实测出来的
 * ============================================================
 *
 * 下面每条序列都先用探针跑过、确认交叉发生在预期的区域，再写进断言 ——
 * 不是猜的。注释里记着实测值，便于日后核对。
 */

#include <gtest/gtest.h>

#include "backtest/strategy_context.h"
#include "strategies/kdj_strategy.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace backtest;

namespace {

Bar bar_at(int i, double px) {
    Bar b;
    char d[32];
    std::snprintf(d, sizeof d, "2025-%02d-%02d", 1 + (i / 28) % 12, 1 + (i % 28));
    b.date = d;
    b.open = px;
    b.high = px + 0.5;
    b.low = px - 0.5;
    b.close = px;
    b.volume = 1000000.0;
    return b;
}

// 把一条序列逐 bar 喂给策略，返回全部订单。
// has_position 按已收到的买/卖单模拟，因为区域过滤的分支依赖 ctx.has_position()。
std::vector<Order> run(KDJStrategy& s, const std::vector<Bar>& bars) {
    std::vector<Order> all;
    int pos = 0;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        std::vector<Bar> history(bars.begin(), bars.begin() + static_cast<long>(i) + 1);

        StrategyContext ctx;
        ctx.symbol = "TEST";
        ctx.bar_index = static_cast<int>(i);
        ctx.current_bar = bars[i];
        ctx.history = &history;
        ctx.cash = 100000.0;
        ctx.position_quantity = pos;
        ctx.position_avg_price = pos > 0 ? 100.0 : 0.0;
        ctx.total_value = 100000.0;
        ctx.lot_size = 100;

        for (auto& o : s.on_bar(ctx)) {
            if (o.side == Side::BUY) pos += o.quantity;
            else pos = 0;
            all.push_back(o);
        }
    }
    return all;
}

int count(const std::vector<Order>& os, Side side) {
    int n = 0;
    for (const auto& o : os) if (o.side == side) ++n;
    return n;
}

/*
 * 序列 A：深跌 → 猛涨 → 回落。
 * 实测（n=9,m1=3,m2=3）：bar 19 金叉 k=9.66（超卖区）、bar 35 死叉 k=92.76（超买区）。
 * 这是一个在**正确语义下**完整的低位买入 → 高位卖出往返。
 */
std::vector<Bar> deep_dip_then_rally_then_fade() {
    std::vector<Bar> v;
    for (int i = 0; i < 18; ++i) v.push_back(bar_at(i, 100.0 - i * 3.0));        // 跌到 49
    for (int i = 0; i < 16; ++i) v.push_back(bar_at(18 + i, 49.0 + i * 4.0));    // 涨到 109
    for (int i = 0; i < 8; ++i)  v.push_back(bar_at(34 + i, 109.0 - i * 4.0));   // 回落
    return v;
}

/*
 * 序列 B：浅幅正弦震荡。
 * 实测：金叉发生在 bar 17 (k=20.00)、36 (k=22.86)、55 (k=25.26)，
 * 死叉发生在 bar 27 (k=69.82)、45 (k=83.09)。
 *
 * 关键点：**没有任何一次金叉发生在 k < 20 的超卖区**。
 * 所以正确实现在这条序列上不该买；而原来那份写反的实现会买（k < 80 恒真）。
 * 这是本文件里最重要的一条判别性测试。
 */
std::vector<Bar> shallow_oscillation() {
    std::vector<Bar> v;
    for (int i = 0; i < 60; ++i) v.push_back(bar_at(i, 100.0 + 6.0 * std::sin(i / 3.0)));
    return v;
}

}  // namespace

// ── 基本信息 ──

TEST(KDJStrategyTest, NameAndParamSchema) {
    KDJStrategy s;
    EXPECT_EQ(s.name(), "KDJ");
    const auto schema = s.param_schema();
    // 文档里承诺的两个区域参数必须在 schema 里
    EXPECT_EQ(schema.count("oversold"), 1u);
    EXPECT_EQ(schema.count("overbought"), 1u);
}

/*
 * ── 正确语义：低位金叉买入、高位死叉卖出 ──
 *
 * 这条测试在修复前后**都通过** —— 它验证的是「该做的事还在做」，
 * 防止修复把正常路径一起改坏。判别性在下面两条。
 */
TEST(KDJStrategyTest, BuysOnGoldenCrossInOversoldZoneAndSellsOnDeathCrossInOverboughtZone) {
    KDJStrategy s;   // 默认 oversold=20, overbought=80
    const auto orders = run(s, deep_dip_then_rally_then_fade());

    ASSERT_EQ(count(orders, Side::BUY), 1)
        << "bar 19 有一次 k=9.66 的金叉，落在超卖区（<20），应当买入一次";
    ASSERT_EQ(count(orders, Side::SELL), 1)
        << "bar 35 有一次 k=92.76 的死叉，落在超买区（>80），应当卖出一次";
    // 顺序必须是先买后卖
    EXPECT_EQ(orders.front().side, Side::BUY);
    EXPECT_EQ(orders.back().side, Side::SELL);
}

/*
 * ── 判别性测试 1：区域过滤必须真的生效 ──
 *
 * 浅幅震荡序列上，五次交叉全部发生在 (20, 80) 中间区，**没有一次金叉在超卖区**。
 *
 * 正确实现：不买。
 * 原来那份写反的实现：`k < overbought_`（k < 80）对三次金叉全部成立 → 会买。
 *
 * 所以这条测试在修复前是红的，修复后是绿的。区域过滤如果哪天又被改成恒真，
 * 它会立刻变红。
 */
TEST(KDJStrategyTest, DoesNotBuyWhenGoldenCrossHappensOutsideOversoldZone) {
    KDJStrategy s;   // oversold=20
    const auto orders = run(s, shallow_oscillation());

    EXPECT_EQ(count(orders, Side::BUY), 0)
        << "这条序列的金叉发生在 k≈20.0 / 22.9 / 25.3，都不在超卖区（<20）内，\n"
           "不应产生任何买入。若这里出现买单，说明区域过滤又失效了 ——\n"
           "最典型的失效方式就是把 oversold_ 和 overbought_ 用反。";
}

/*
 * ── 判别性测试 2：两个参数各自管哪一边，必须与文档一致 ──
 *
 * 用一组**不对称**的阈值把两种实现彻底分开：oversold=30、overbought=99。
 * 浅幅震荡序列的交叉实测在 k≈20.0 / 22.9 / 25.3（金叉）与 69.8 / 83.1（死叉）。
 *
 *   正确实现：金叉看 k < oversold(30) → 三次都放行，会买；
 *             死叉看 k > overbought(99) → 一次都不到，**永远不卖**。
 *   写反的实现：金叉看 k < overbought(99) → 也会买（这一半不判别）；
 *             死叉看 k > oversold(30) → 69.8 和 83.1 都放行，**会卖**。
 *
 * 所以判别点在「卖」那一边：把 overbought 设到 99 就等于禁止卖出，
 * 出现卖单就说明卖出过滤器读的是 oversold —— 两个参数又被互换了。
 */
TEST(KDJStrategyTest, EachThresholdGovernsItsOwnSideAsDocumented) {
    KDJStrategy s(9, 3, 3, /*oversold=*/30.0, /*overbought=*/99.0);
    const auto orders = run(s, shallow_oscillation());

    EXPECT_GT(count(orders, Side::BUY), 0)
        << "oversold 放宽到 30 之后，k≈20~25 的那几次金叉都应当被放行。\n"
           "买不进去说明买入过滤读的不是 oversold。";

    EXPECT_EQ(count(orders, Side::SELL), 0)
        << "overbought 设到 99 等于禁止卖出（这条序列的 k 最高只到 83）。\n"
           "出现卖单说明卖出过滤读的是 oversold(30) —— 两个阈值被互换了。";
}

/*
 * ── 首个被评估的 bar 不得凭虚构前值造出交叉 ──
 *
 * prev_k_ / prev_d_ 的初值都是 50.0，而 bar_index < n_ 的早退分支不更新它们。
 * 所以在第一个真正被评估的 bar（默认 n=9 时是 bar 9）上，
 * `prev_k_ <= prev_d_` 与 `prev_k_ >= prev_d_` **同时为真** ——
 * 没有守卫的话，那一根一定会产出一个针对虚构前值的假交叉。
 *
 * 这里刻意把 oversold 设成 95，让阈值过滤不挡路，从而**只**检验守卫本身：
 * 单调上涨序列在 bar 9 上 k > d 且 k < 95，没有守卫就会买；有守卫则不买。
 *
 * MACDStrategy 一直有这个守卫（macd_strategy.cpp:44-49），KDJ 漏了。
 */
TEST(KDJStrategyTest, FirstEvaluatedBarDoesNotFabricateACrossFromSeededPrevValues) {
    std::vector<Bar> rising;
    for (int i = 0; i < 12; ++i) rising.push_back(bar_at(i, 100.0 + i * 3.0));

    KDJStrategy s(9, 3, 3, /*oversold=*/95.0, /*overbought=*/99.0);

    // 只看第一个被评估的 bar（index 9）
    std::vector<Bar> history(rising.begin(), rising.begin() + 10);
    StrategyContext ctx;
    ctx.symbol = "TEST";
    ctx.bar_index = 9;
    ctx.current_bar = rising[9];
    ctx.history = &history;
    ctx.cash = 100000.0;
    ctx.position_quantity = 0;
    ctx.total_value = 100000.0;
    ctx.lot_size = 100;

    // 前提确认：这一根确实 k > d（否则这条测试测不到守卫）
    const auto kd = ctx.kdj(9, 3, 3);
    ASSERT_GT(kd.k, kd.d)
        << "构造前提失效：bar 9 需要 k > d，虚构的 50/50 前值才会被判成金叉。"
           "序列或参数变了，需要重新构造。";
    ASSERT_LT(kd.k, 95.0) << "构造前提失效：k 需要落在放宽后的买入阈值之内";

    EXPECT_TRUE(s.on_bar(ctx).empty())
        << "第一个被评估的 bar 只应播种 prev_k_/prev_d_，不应下单。\n"
           "这里出现订单说明 initialized_ 守卫没了 —— 那是一个基于 50.0 初值的假交叉。";
}
