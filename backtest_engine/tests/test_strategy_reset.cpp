/*
 * test_strategy_reset.cpp — 策略的跨场 / 跨标的状态隔离
 *
 * ============================================================
 * 这个文件守的是什么
 * ============================================================
 *
 * 指标本身的增量状态（IndicatorState）是干净的：它挂在 engine.cpp 里 run()
 * 的局部变量 `state` 上，每场回测天然是新的。
 *
 * 缺口在**策略层**。这五个策略在成员变量里存了跨 bar 的状态：
 *
 *     ma_cross_strategy.h   prev_fast_ma_ / prev_slow_ma_
 *     macd_strategy.h       prev_dif_ / prev_dea_ / initialized_
 *     rsi_strategy.h        prev_rsi_
 *     kdj_strategy.h        prev_k_ / prev_d_ / initialized_
 *     bollinger_strategy.h  prev_close_ / prev_lower_ / prev_upper_
 *
 * 而九个策略里**只有 PairsStrategy 覆写了 on_init()**（它清 spreads_）。
 * engine.cpp 每场 run() 开头都调用 strategy_->on_init()，钩子一直是现成的，
 * 但没有一个带状态的策略用它。
 *
 * 于是有两类 bug，都不会在单次回测里暴露：
 *
 *   ① 跨场串味 —— 同一个策略对象先用过一次，再用第二次，第一个被评估的 bar
 *      就拿着上一场的 prev_ 值去判交叉。服务端复用策略对象就是这个形态。
 *      benchmarks/bench_backtest.cpp 的注释其实早就知道这件事（「复用同一个
 *      实例跑第二遍，结果会和第一遍不同」），靠每次新建实例绕开，从没测试钉住。
 *
 *   ② 跨标的串味 —— 组合回测下**所有标的共用同一个策略实例**
 *      （engine.cpp::set_strategy 的注释明确写了这一点）。于是 A 的 prev_dif_
 *      被 B 覆盖，两个标的互相污染。
 *      ⚠️ 同一处注释还断言「现有 9 个策略都是从 ctx.history 现算的，故安全」——
 *      这句话与上面五个 .h 的代码事实不符。本文件把它证伪。
 *
 * ============================================================
 * 为什么两组都在策略层测，不走引擎
 * ============================================================
 *
 * 组合回测里现金是全场共享的，A 先花掉的钱 B 就看不到了。所以「两个标的一起跑」
 * 与「各自单独跑」的成交本来就会不同 —— 那是**正当的资金竞争**，不是串味。
 * 走引擎会把两种效应混在一起，判据就不干净了。
 *
 * 策略层直接比**下单流**：同样的 context 序列喂进去，下的单必须一模一样。
 * 持仓由测试自己按策略发出的单跟踪，好让买卖两条分支都可达
 * （只喂「空仓」的话，死叉分支永远走不到，测试的敏感度会少一半）。
 *
 * ============================================================
 * 行情为什么长这个样子：污染要显形是有条件的
 * ============================================================
 *
 * 第一版用的是一条普通的正弦波，结果**两组都绿**。查下来原因很实在：
 *
 *   跨场污染只能影响**第一个被评估的 bar**。那一根过后 prev_ 就被本场的值
 *   刷新，脏实例与干净实例此后完全同步。所以只有「上一场的末值 与 本场首个
 *   评估 bar 的值 恰好构成一次**金叉**」时才看得见 —— 死叉要有持仓，而第二场
 *   开局是空仓，走不到。
 *
 * 所以 warm-up 那段必须**以看跌状态收尾**（prev 落在下方），被测那段必须
 * **以看涨状态开局**。于是：
 *
 *   warm-up = falling_then_plunge  线性下跌 + 末尾急跌
 *             急跌段是给 BOLLINGER 的：线性下跌里 close 比下轨还高约 0.61，
 *             `prev_close_ <= prev_lower_` 不成立，抓不住；跌破下轨才成立。
 *   被测    = dip_then_rise        先跌到第 8 根触底，再上涨，之后转正弦波
 *             触底回升让均线/MACD/RSI 在各自的首个评估 bar 上都呈金叉形态。
 *
 * ⚠️ **KDJ 在跨场这一组里是结构免疫的，抓不住不是 fixture 没调好。**
 *    k 从 50 起步，到第一个被评估的 bar（bar_index = n_ = 9）只走了两步：
 *        k₈ = (rsv₈ + 2·50)/3        ∈ [33.33, 66.67]
 *        k₉ = (rsv₉ + 2·k₈)/3        ∈ [22.22, 77.78]
 *    而金叉要 k < 20、死叉要 k > 80 —— **两条都够不着**，与数据无关。
 *    也就是说 KDJ 的区域过滤恰好挡住了这个 bug 在单标的跨场下的唯一出口。
 *    跨标的那一组里 KDJ 照样会红：那里污染发生在**每一根 bar**，
 *    k 早就跑出 [22, 78] 了。
 *
 *    断言仍然对五个策略一起下：它是正确的不变量，修完必须全绿。
 *    抓不住的那个在这里写明原因，免得后人以为它被覆盖了。
 */

#include <gtest/gtest.h>

#include "backtest/strategy_base.h"
#include "backtest/strategy_context.h"
#include "backtest/types.h"
#include "strategies/bollinger_strategy.h"
#include "strategies/kdj_strategy.h"
#include "strategies/ma_cross_strategy.h"
#include "strategies/macd_strategy.h"
#include "strategies/rsi_strategy.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace backtest;

namespace {

std::string day(int i) {
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d",
                  2025 + i / (12 * 28), 1 + (i % (12 * 28)) / 28, 1 + (i % 28));
    return b;
}

Bar mk(int i, double px) {
    Bar b;
    b.date = day(i);
    b.close = px;
    b.open = px - 0.15;
    b.high = px + 0.45;
    b.low = px - 0.45;
    b.volume = 1000000.0;
    return b;
}

/* 线性下跌 + 末尾急跌（急跌段让 close 跌破布林下轨，理由见文件头） */
std::vector<Bar> falling_then_plunge(int n, double start, int plunge) {
    std::vector<Bar> v;
    v.reserve(static_cast<size_t>(n));
    double px = start;
    for (int i = 0; i < n; ++i) {
        px -= (i >= n - plunge) ? 6.0 : 0.30;
        v.push_back(mk(i, px));
    }
    return v;
}

/* 先跌到 bottom_at 触底，再涨到第 70 根，之后转正弦波（保证后段有真实交易） */
std::vector<Bar> dip_then_rise(int n, double base, int bottom_at) {
    std::vector<Bar> v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double px;
        if (i <= bottom_at) {
            px = base - 1.2 * i;
        } else if (i <= 70) {
            px = base - 1.2 * bottom_at + 1.6 * (i - bottom_at);
        } else {
            px = base - 1.2 * bottom_at + 1.6 * (70 - bottom_at)
                 + 12.0 * std::sin((i - 70) / 7.0)
                 + 5.0 * std::sin((i - 70) / 23.0);
        }
        v.push_back(mk(i, px));
    }
    return v;
}

using Factory = std::function<std::unique_ptr<IStrategy>()>;

struct Case {
    const char* name;
    Factory make;
};

std::vector<Case> stateful_strategies() {
    return {
        {"MA_CROSS",  [] { return std::unique_ptr<IStrategy>(
                               new MACrossStrategy(5, 20, 0.95)); }},
        {"MACD",      [] { return std::unique_ptr<IStrategy>(
                               new MACDStrategy(12, 26, 9, 0.95)); }},
        {"RSI",       [] { return std::unique_ptr<IStrategy>(
                               new RSIStrategy(14, 30.0, 70.0, 0.95)); }},
        {"KDJ",       [] { return std::unique_ptr<IStrategy>(
                               new KDJStrategy(9, 3, 3, 20.0, 80.0, 0.95)); }},
        {"BOLLINGER", [] { return std::unique_ptr<IStrategy>(
                               new BollingerStrategy(20, 2.0, 0.95)); }},
    };
}

/* 单根 bar 的 context；持仓由调用方维护，好让买卖两条分支都可达 */
StrategyContext context_at(const std::string& symbol, const std::vector<Bar>& hist,
                           IndicatorState& ind, size_t i, int pos) {
    StrategyContext ctx;
    ctx.symbol = symbol;
    ctx.bar_index = static_cast<int>(i);
    ctx.current_bar = hist.back();
    ctx.history = &hist;
    ctx.indicators = &ind;
    ctx.cash = 1000000.0;          // 现金给足，排除资金竞争这个变量
    ctx.total_value = 1000000.0;
    ctx.lot_size = 100;
    ctx.position_quantity = pos;
    ctx.position_avg_price = pos ? hist.back().close : 0.0;
    return ctx;
}

/*
 * 把一段行情喂给策略，返回它下的单（"bar序号:数量;" 拼起来）。
 *
 * 每次调用用**全新**的 history 与 IndicatorState：要测的是策略成员里的状态，
 * 指标状态必须是干净的，否则两个变量混在一起。
 */
std::string order_stream(IStrategy* s, const std::vector<Bar>& bars,
                         const std::string& symbol = "X") {
    s->on_init();               // 引擎每场 run() 开头就是这么做的（engine.cpp:139）

    std::vector<Bar> hist;
    hist.reserve(bars.size());
    IndicatorState ind;
    std::string out;
    int pos = 0;

    for (size_t i = 0; i < bars.size(); ++i) {
        hist.push_back(bars[i]);
        for (const auto& o : s->on_bar(context_at(symbol, hist, ind, i, pos))) {
            out += std::to_string(i) + ":" + std::to_string(o.quantity) + ";";
            pos += (o.quantity > 0) ? o.quantity : -pos;
        }
    }
    return out;
}

size_t count_orders(const std::string& stream) {
    size_t n = 0;
    for (char ch : stream) if (ch == ';') ++n;
    return n;
}

}  // namespace

/*
 * ============================================================
 * ① 跨场：用过一次的策略对象，再用一次必须等于全新的
 * ============================================================
 */
TEST(StrategyResetTest, ReusedInstanceBehavesLikeAFreshOne) {
    const auto warmup = falling_then_plunge(300, 200.0, 5);
    const auto under_test = dip_then_rise(400, 150.0, 8);

    for (const auto& c : stateful_strategies()) {
        auto fresh = c.make();
        const std::string expected = order_stream(fresh.get(), under_test);

        auto reused = c.make();
        order_stream(reused.get(), warmup);              // 先用过一次
        const std::string actual = order_stream(reused.get(), under_test);

        EXPECT_EQ(actual, expected)
            << c.name << "：用过一次的策略对象再跑一段行情，下单流与全新实例不同 —— "
               "上一场的 prev_ 状态被带了进来。\n"
            << "  全新实例 " << count_orders(expected) << " 单 / 复用实例 "
            << count_orders(actual) << " 单\n"
            << "  修法：覆写 on_init()，把 prev_* / initialized_ 清回初值。"
               "engine.cpp 每场 run() 开头都会调它，钩子是现成的。";
    }
}

/*
 * ② 同一份数据连跑两遍 —— 「跑两遍，第二遍必须等于单独跑一遍」这条口径。
 *
 * ⚠️ 这一条比上面那条**弱**：同一份数据时，上一场的末值与本场首个评估 bar
 *    的值未必构成金叉，缺陷可能蒙混过去（实测在好几份行情上就是绿的）。
 *    留着它是因为这是最直观的口径，但真正的判据是上面那条。
 */
TEST(StrategyResetTest, RunningTheSameDataTwiceMatchesASingleRun) {
    const auto bars = dip_then_rise(400, 150.0, 8);

    for (const auto& c : stateful_strategies()) {
        auto fresh = c.make();
        const std::string expected = order_stream(fresh.get(), bars);

        auto twice = c.make();
        order_stream(twice.get(), bars);                 // 第一遍
        const std::string actual = order_stream(twice.get(), bars);

        EXPECT_EQ(actual, expected)
            << c.name << "：同一份数据跑第二遍，结果与单独跑一遍不同。";
    }
}

/*
 * ============================================================
 * ③ 跨标的：一个实例服务两个标的，不能互相污染
 * ============================================================
 */
TEST(StrategyResetTest, OneInstanceServingTwoSymbolsDoesNotContaminate) {
    // 两个标的**必须是不同的行情**：数据相同的话，被污染的前值恰好等于
    // 正确的前值，bug 会完全隐形。
    const auto bars_a = dip_then_rise(400, 150.0, 8);
    const auto bars_b = dip_then_rise(400, 300.0, 20);

    for (const auto& c : stateful_strategies()) {
        // 参照：两个独立实例，各自只看自己的标的
        auto solo_a = c.make();
        auto solo_b = c.make();
        const std::string want_a = order_stream(solo_a.get(), bars_a, "AAA.SH");
        const std::string want_b = order_stream(solo_b.get(), bars_b, "BBB.SH");

        // 被测：一个实例，A/B 交替 —— 组合回测里就是这样的
        auto shared = c.make();
        // ⚠️ 一场回测只调一次 on_init()，然后两个标的在这一场里交替 ——
        //    组合回测就是这个形态。所以 on_init() 单独**解决不了**跨标的串味，
        //    状态必须按标的分开存。
        shared->on_init();
        std::vector<Bar> hist_a, hist_b;
        IndicatorState ind_a, ind_b;      // 指标状态本来就是每标的一份
        std::string got_a, got_b;
        int pos_a = 0, pos_b = 0;

        auto step = [&](const std::vector<Bar>& src, std::vector<Bar>& hist,
                        IndicatorState& ind, const std::string& sym,
                        std::string& sink, int& pos, size_t i) {
            hist.push_back(src[i]);
            for (const auto& o : shared->on_bar(context_at(sym, hist, ind, i, pos))) {
                sink += std::to_string(i) + ":" + std::to_string(o.quantity) + ";";
                pos += (o.quantity > 0) ? o.quantity : -pos;
            }
        };

        for (size_t i = 0; i < bars_a.size(); ++i) {
            step(bars_a, hist_a, ind_a, "AAA.SH", got_a, pos_a, i);
            step(bars_b, hist_b, ind_b, "BBB.SH", got_b, pos_b, i);
        }

        EXPECT_EQ(got_a, want_a)
            << c.name << " / 标的 A：被另一个标的污染了。独立 "
            << count_orders(want_a) << " 单，共用实例 " << count_orders(got_a) << " 单。\n"
            << "  组合回测下所有标的共用一个策略实例（engine.cpp::set_strategy），"
               "跨 bar 状态存在成员变量里就会串味。\n"
            << "  ⚠️ engine.cpp 那句「现有 9 个策略都是从 ctx.history 现算的，故安全」"
               "与代码事实不符 —— 这五个策略都存了 prev_*。";
        EXPECT_EQ(got_b, want_b)
            << c.name << " / 标的 B：被另一个标的污染了。独立 "
            << count_orders(want_b) << " 单，共用实例 " << count_orders(got_b) << " 单。";
    }
}

/*
 * ============================================================
 * ④ 元测试：上面几个用例确实在测东西
 * ============================================================
 *
 * 行情太平、策略一笔都不下的话，上面的用例会以「两边都是空」的方式假绿。
 */
TEST(StrategyResetTest, FixturesActuallyProduceOrders) {
    const auto under_test = dip_then_rise(400, 150.0, 8);
    const auto other = dip_then_rise(400, 300.0, 20);

    for (const auto& c : stateful_strategies()) {
        auto a = c.make();
        EXPECT_GT(count_orders(order_stream(a.get(), under_test)), 0u)
            << c.name << "：被测行情上一单未下，隔离测试会假绿。";
        auto b = c.make();
        EXPECT_GT(count_orders(order_stream(b.get(), other)), 0u)
            << c.name << "：第二个标的的行情上一单未下，跨标的测试会假绿。";
    }
}
