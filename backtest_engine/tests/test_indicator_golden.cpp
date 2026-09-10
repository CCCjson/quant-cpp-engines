/*
 * test_indicator_golden.cpp — 指标金标准测试
 *
 * ============================================================
 * 这个文件守的是什么
 * ============================================================
 *
 * 在这个文件出现之前，仓库里**没有任何测试断言过任何指标的值**：
 * macd / rsi / kdj / ema / bollinger / stddev 全部零值断言，
 * tests/ 里只有 sma() 和 returns() 有测试（test_strategies.cpp:220-259）。
 *
 * 这是个大洞，因为 benchmarks/ 里最有分量的那条结论正是关于这些指标的：
 * 「把 macd()/rsi()/kdj() 从 O(N²) 的全量重算改成增量递推，提速 277×，
 *   而且逐位等价」。
 *
 * 「逐位等价」是那条结论的全部分量。差不多相等的话，那就只是一次普通优化
 * 外加一个「回测数字可能变了」的风险；只有逐位相同，才能说「算的是同一件事，
 * 只是算得快了」。而在这个文件之前，没有任何东西守着这个性质。
 *
 * 现在有了：fixture 是**改动实现之前**从朴素实现导出的，逐根 bar 记录。
 * 任何让指标数值发生一丝变化的改动，都会在这里变红。
 *
 * ============================================================
 * 为什么不用 EXPECT_DOUBLE_EQ
 * ============================================================
 *
 * EXPECT_DOUBLE_EQ 走 gtest 的 AlmostEquals，**容许 4 个 ULP 的误差**。
 * 而我们要防的恰恰就是 1 ULP 级别的偏移——比如 FMA 收缩带来的那种
 * （见 CMakeLists 里 -ffp-contract=off 的说明：同一行递推式，收缩与不收缩
 * 相差 1 ULP）。用 EXPECT_DOUBLE_EQ 等于把要抓的东西放进了容差里。
 *
 * EXPECT_EQ 直接比 double 也不行：-0.0 == 0.0 为真、NaN == NaN 为假，
 * 而且会触发 -Wfloat-equal。
 *
 * 所以比 8 字节位模式的整数值。memcpy 而不是 reinterpret_cast——后者是
 * 严格别名违规，本项目开了 UBSan 会被抓。
 */

#include <gtest/gtest.h>

#include "backtest/strategy_context.h"
#include "backtest/data_loader.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

using namespace backtest;
using nlohmann::json;

namespace {

std::uint64_t bits(double d) {
    std::uint64_t u = 0;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

// 十六进制位模式 → 整数。fixture 里存的就是这个形式。
std::uint64_t from_hex(const std::string& h) {
    return std::stoull(h, nullptr, 16);
}

double as_double(std::uint64_t u) {
    double d = 0.0;
    std::memcpy(&d, &u, sizeof d);
    return d;
}

/*
 * 逐位相等断言。
 * 失败时把两侧的十进制值和位模式都打出来——只报「不相等」的话，
 * 无法判断是差 1 ULP（说明是浮点收缩之类的编译问题）还是差得很远
 * （说明是算法写错了）。这个区分对定位问题很关键。
 */
#define EXPECT_BITWISE_EQ(actual, expected_bits, what)                        \
    do {                                                                      \
        const std::uint64_t a_ = bits(actual);                                \
        const std::uint64_t e_ = (expected_bits);                             \
        EXPECT_EQ(a_, e_)                                                      \
            << "  " << (what) << " 逐位不符\n"                                 \
            << "    实测 = " << std::setprecision(17) << (actual)              \
            << "  (0x" << std::hex << a_ << std::dec << ")\n"                 \
            << "    金标 = " << std::setprecision(17) << as_double(e_)         \
            << "  (0x" << std::hex << e_ << std::dec << ")\n"                 \
            << "    ULP 差 = "                                                 \
            << (a_ > e_ ? a_ - e_ : e_ - a_)                                   \
            << "（差 1-2 ULP 通常是浮点收缩/优化选项问题，"                      \
               "差很多则是算法本身变了）";                                       \
    } while (0)

const json& golden() {
    static const json g = [] {
        std::ifstream in(GOLDEN_FIXTURE_PATH);
        // 用 ASSERT 不行（这里不在测试体内），所以直接抛
        if (!in) {
            throw std::runtime_error(
                std::string("打不开指标金标准 fixture: ") + GOLDEN_FIXTURE_PATH +
                "\n如果它不存在，先跑：./backtest_engine/build/indicator_golden_gen"
                " > backtest_engine/tests/data/indicator_golden.json");
        }
        json j;
        in >> j;
        return j;
    }();
    return g;
}

Bar mk(const std::string& date, double open, double high, double low, double close) {
    Bar b;
    b.date = date;
    b.open = open;
    b.high = high;
    b.low = low;
    b.close = close;
    b.volume = 1000000.0;
    return b;
}

std::string day(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "2025-%02d-%02d", (i / 28) + 1, (i % 28) + 1);
    return buf;
}

// 与 gen_indicator_golden.cpp 里三条构造序列的定义必须完全一致
std::vector<Bar> flat_series() {
    std::vector<Bar> v;
    for (int i = 0; i < 60; ++i) v.push_back(mk(day(i), 50.0, 50.0, 50.0, 50.0));
    return v;
}
std::vector<Bar> rising_series() {
    std::vector<Bar> v;
    for (int i = 0; i < 60; ++i) {
        double up = 100.0 + i;
        v.push_back(mk(day(i), up, up + 0.5, up - 0.5, up));
    }
    return v;
}
std::vector<Bar> falling_series() {
    std::vector<Bar> v;
    for (int i = 0; i < 60; ++i) {
        double dn = 200.0 - i;
        v.push_back(mk(day(i), dn, dn + 0.5, dn - 0.5, dn));
    }
    return v;
}

std::vector<Bar> real_series() {
    std::ifstream in(GOLDEN_BARS_PATH);
    if (!in) throw std::runtime_error(std::string("打不开真实日线: ") + GOLDEN_BARS_PATH);
    json raw;
    in >> raw;
    return DataLoader::from_json(raw);
}

/*
 * 核心比对：逐根 bar 重算，与 fixture 逐位比。
 *
 * ctx.history 指向一个逐步增长的 vector，与引擎里的真实情形一致
 * （engine.cpp:354-374 先 push_back 今天的 bar 再构造 context）。
 * 指标种子取自 history[0]，所以数值依赖回测起点——这个性质也被一起钉住。
 */
void check_series(const std::string& key, const std::vector<Bar>& bars) {
    const json& rows = golden().at("series").at(key);
    ASSERT_EQ(rows.size(), bars.size())
        << key << ": fixture 的 bar 数与序列长度不符，fixture 可能过期了";

    std::vector<Bar> history;
    history.reserve(bars.size());

    for (size_t i = 0; i < bars.size(); ++i) {
        history.push_back(bars[i]);

        StrategyContext ctx;
        ctx.symbol = key;
        ctx.bar_index = static_cast<int>(i);
        ctx.current_bar = bars[i];
        ctx.history = &history;

        const json& g = rows[i];
        ASSERT_EQ(g.at("i").get<size_t>(), i) << key << ": fixture 行序错位";

        const std::string tag = key + " bar#" + std::to_string(i) +
                                " (" + bars[i].date + ") ";

        const auto m = ctx.macd(12, 26, 9);
        EXPECT_BITWISE_EQ(m.dif, from_hex(g.at("macd_dif")), tag + "macd.dif");
        EXPECT_BITWISE_EQ(m.dea, from_hex(g.at("macd_dea")), tag + "macd.dea");
        EXPECT_BITWISE_EQ(m.hist, from_hex(g.at("macd_hist")), tag + "macd.hist");

        EXPECT_BITWISE_EQ(ctx.rsi(14), from_hex(g.at("rsi14")), tag + "rsi(14)");

        const auto k = ctx.kdj(9, 3, 3);
        EXPECT_BITWISE_EQ(k.k, from_hex(g.at("kdj_k")), tag + "kdj.k");
        EXPECT_BITWISE_EQ(k.d, from_hex(g.at("kdj_d")), tag + "kdj.d");
        EXPECT_BITWISE_EQ(k.j, from_hex(g.at("kdj_j")), tag + "kdj.j");

        const auto bb = ctx.bollinger(20, 2.0);
        EXPECT_BITWISE_EQ(bb.upper, from_hex(g.at("boll_upper")), tag + "boll.upper");
        EXPECT_BITWISE_EQ(bb.middle, from_hex(g.at("boll_middle")), tag + "boll.middle");
        EXPECT_BITWISE_EQ(bb.lower, from_hex(g.at("boll_lower")), tag + "boll.lower");

        EXPECT_BITWISE_EQ(ctx.sma(5), from_hex(g.at("sma5")), tag + "sma(5)");
        EXPECT_BITWISE_EQ(ctx.sma(20), from_hex(g.at("sma20")), tag + "sma(20)");
        EXPECT_BITWISE_EQ(ctx.stddev(20), from_hex(g.at("stddev20")), tag + "stddev(20)");
    }
}

}  // namespace

// ── 真实日线：180 根，与 parity 门禁用的是同一份 fixture ──
TEST(GoldenIndicatorTest, RealBars180) {
    check_series("real_180", real_series());
}

/*
 * ── 三条构造序列：专打重写时最容易悄悄改掉的分支 ──
 *
 * flat：high == low == close，逼出 kdj() 里 `(hhv == llv) ? 50.0 : ...` 的
 * **精确相等**判断（strategy_context.h:350）。任何把它改成带 epsilon 的
 * 「改进」都会让这个用例变红——这正是想要的效果，因为那会改变回测结果。
 */
TEST(GoldenIndicatorTest, FlatSeriesHitsKdjEqualHighLowBranch) {
    check_series("flat_60", flat_series());
}

// rising：单调上涨，逼出 rsi() 的 `avg_loss == 0.0 → return 100.0`（:316）
TEST(GoldenIndicatorTest, RisingSeriesHitsRsiZeroLossBranch) {
    check_series("rising_60", rising_series());
}

// falling：单调下跌，逼出 avg_gain 为 0 的另一侧
TEST(GoldenIndicatorTest, FallingSeriesHitsRsiZeroGainBranch) {
    check_series("falling_60", falling_series());
}

/*
 * ── 元测试：确认金标准本身覆盖了该覆盖的东西 ──
 *
 * 一个全是 0 或者只有几根 bar 的 fixture 也能让上面四个用例通过。
 * 这个用例断言 fixture 的形状与关键分支的取值，防止「fixture 退化了但
 * 测试依然全绿」这种最坏情况。
 */
TEST(GoldenIndicatorTest, FixtureActuallyCoversTheInterestingBranches) {
    const json& s = golden().at("series");

    EXPECT_EQ(s.at("real_180").size(), 180u);
    EXPECT_EQ(s.at("flat_60").size(), 60u);

    // 预热哨兵：第 0 根 bar 上 macd 报 0、rsi 和 kdj 报中性值
    const json& r0 = s.at("real_180")[0];
    EXPECT_EQ(as_double(from_hex(r0.at("macd_dif"))), 0.0) << "预热期 macd 应报 0";
    EXPECT_EQ(as_double(from_hex(r0.at("rsi14"))), 50.0) << "数据不足时 rsi 应报中性 50";
    EXPECT_EQ(as_double(from_hex(r0.at("kdj_k"))), 50.0) << "数据不足时 kdj 应报中性 50";

    // 正常期确实是非零的（防 fixture 整体退化成 0）
    const json& r179 = s.at("real_180")[179];
    EXPECT_NE(as_double(from_hex(r179.at("macd_dif"))), 0.0);
    EXPECT_NE(as_double(from_hex(r179.at("rsi14"))), 50.0);

    // 三条边界分支确实被打中
    EXPECT_EQ(as_double(from_hex(s.at("flat_60")[30].at("kdj_k"))), 50.0)
        << "flat 序列应命中 kdj 的 hhv==llv 分支";
    EXPECT_EQ(as_double(from_hex(s.at("rising_60")[30].at("rsi14"))), 100.0)
        << "rising 序列应命中 rsi 的 avg_loss==0 分支";
    EXPECT_EQ(as_double(from_hex(s.at("falling_60")[30].at("rsi14"))), 0.0)
        << "falling 序列应命中 rsi 的 avg_gain==0 一侧";
}
