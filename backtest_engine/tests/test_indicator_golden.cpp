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

#include "backtest/indicators.h"
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
    char buf[32];
    std::snprintf(buf, sizeof buf, "2025-%02d-%02d", 1 + (i / 28) % 12, 1 + (i % 28));
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
/*
 * mode = Naive       —— ctx.indicators 为空，走全量重算的回退路径
 * mode = Incremental —— 挂上 IndicatorState，走增量递推路径
 *
 * **两条路径都必须逐位命中同一份金标准。** 这正是「增量重写逐位等价」这句话
 * 的全部内容：fixture 是在改动之前从朴素实现导出的，如果增量版有一位不同，
 * 下面就会红。
 */
enum class Mode { Naive, Incremental };

void check_series(const std::string& key, const std::vector<Bar>& bars,
                  Mode mode = Mode::Naive) {
    const json& rows = golden().at("series").at(key);
    ASSERT_EQ(rows.size(), bars.size())
        << key << ": fixture 的 bar 数与序列长度不符，fixture 可能过期了";

    std::vector<Bar> history;
    history.reserve(bars.size());

    // 增量模式下，这份状态要跨 bar 存活（模拟引擎里挂在 SymbolState 上的那份）
    IndicatorState istate;

    for (size_t i = 0; i < bars.size(); ++i) {
        history.push_back(bars[i]);

        StrategyContext ctx;
        ctx.symbol = key;
        ctx.bar_index = static_cast<int>(i);
        ctx.current_bar = bars[i];
        ctx.history = &history;
        if (mode == Mode::Incremental) ctx.indicators = &istate;

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
    check_series("real_180", real_series(), Mode::Naive);
}

// ── 同一份金标准，走增量路径 ──
// 这几个用例才是「增量重写逐位等价」的真正证明。上面那组走的是全量重算回退路径，
// 只能说明 fixture 本身没坏。
TEST(GoldenIndicatorTest, RealBars180_Incremental) {
    check_series("real_180", real_series(), Mode::Incremental);
}

/*
 * ── 三条构造序列：专打重写时最容易悄悄改掉的分支 ──
 *
 * flat：high == low == close，逼出 kdj() 里 `(hhv == llv) ? 50.0 : ...` 的
 * **精确相等**判断（strategy_context.h:350）。任何把它改成带 epsilon 的
 * 「改进」都会让这个用例变红——这正是想要的效果，因为那会改变回测结果。
 */
TEST(GoldenIndicatorTest, FlatSeriesHitsKdjEqualHighLowBranch) {
    check_series("flat_60", flat_series(), Mode::Naive);
}

TEST(GoldenIndicatorTest, FlatSeriesHitsKdjEqualHighLowBranch_Incremental) {
    check_series("flat_60", flat_series(), Mode::Incremental);
}

// rising：单调上涨，逼出 rsi() 的 `avg_loss == 0.0 → return 100.0`（:316）
TEST(GoldenIndicatorTest, RisingSeriesHitsRsiZeroLossBranch) {
    check_series("rising_60", rising_series(), Mode::Naive);
}

TEST(GoldenIndicatorTest, RisingSeriesHitsRsiZeroLossBranch_Incremental) {
    check_series("rising_60", rising_series(), Mode::Incremental);
}

// falling：单调下跌，逼出 avg_gain 为 0 的另一侧
TEST(GoldenIndicatorTest, FallingSeriesHitsRsiZeroGainBranch) {
    check_series("falling_60", falling_series(), Mode::Naive);
}

TEST(GoldenIndicatorTest, FallingSeriesHitsRsiZeroGainBranch_Incremental) {
    check_series("falling_60", falling_series(), Mode::Incremental);
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

/*
 * ── 同一根 bar 上重复读取必须幂等 ──
 *
 * 朴素实现是**纯函数**：同一根 bar 调用两次得到同样的值。而 ComboStrategy 会把
 * context 按值复制两份、对每个子策略**每根 bar 调用两次**（combo_strategy.cpp），
 * 所以增量版必须保持这个性质。
 *
 * 说明一下这个用例实际验证了什么、以及没验证什么（实测确认过）：
 *
 * 「重复调用导致递推被多推一次」在当前设计下**结构上不可能发生** ——
 * 步进的条件是 `valid_size == n - 1` 且上一根 bar 指纹吻合，二者**恰好**成立才步进；
 * 同一根 bar 上的第二次调用 valid_size 已经等于 n，落不进步进分支，
 * 而是走全量重建，得到同样正确的值。
 * 我把 `valid_size == n` 那个短路临时删掉验证过：这个用例仍然通过。
 *
 * 也就是说那个短路是**性能优化，不是正确性保证** —— 没有它，
 * ComboStrategy 的第二次调用会退化成 O(N) 重建，正确但慢。
 *
 * 所以这个用例真正钉住的是：**重复读取返回逐位相同的值，且该值仍然是这根 bar 的
 * 金标准值**（没有被推到下一根去）。若将来有人把步进条件改宽（比如写成
 * `valid_size >= n - 1`），它会立刻变红。
 *
 * 顺带覆盖了另一件事：多个不同参数的指标共存于同一份 IndicatorState
 * （slot_for 按 (kind, 参数) 分槽），互不干扰。
 */
TEST(GoldenIndicatorTest, RepeatedReadsWithinOneBarAreIdempotent) {
    const std::vector<Bar> bars = real_series();
    const json& rows = golden().at("series").at("real_180");
    ASSERT_EQ(rows.size(), bars.size());

    std::vector<Bar> history;
    history.reserve(bars.size());
    IndicatorState istate;

    for (size_t i = 0; i < bars.size(); ++i) {
        history.push_back(bars[i]);

        StrategyContext ctx;
        ctx.symbol = "IDEMPOTENT";
        ctx.bar_index = static_cast<int>(i);
        ctx.current_bar = bars[i];
        ctx.history = &history;
        ctx.indicators = &istate;

        // 每根 bar 连读三次，模拟 ComboStrategy 的重复调用
        const auto m1 = ctx.macd(12, 26, 9);
        const auto m2 = ctx.macd(12, 26, 9);
        const auto m3 = ctx.macd(12, 26, 9);
        const double r1 = ctx.rsi(14), r2 = ctx.rsi(14);
        const auto k1 = ctx.kdj(9, 3, 3), k2 = ctx.kdj(9, 3, 3);

        const std::string tag = "bar#" + std::to_string(i) + " ";
        EXPECT_EQ(bits(m1.dif), bits(m2.dif)) << tag << "第二次读 macd.dif 变了 —— 递推被多推了一次";
        EXPECT_EQ(bits(m2.dif), bits(m3.dif)) << tag << "第三次读 macd.dif 又变了";
        EXPECT_EQ(bits(m1.dea), bits(m3.dea)) << tag << "重复读 macd.dea 不幂等";
        EXPECT_EQ(bits(r1), bits(r2)) << tag << "重复读 rsi 不幂等";
        EXPECT_EQ(bits(k1.k), bits(k2.k)) << tag << "重复读 kdj.k 不幂等";
        EXPECT_EQ(bits(k1.d), bits(k2.d)) << tag << "重复读 kdj.d 不幂等";

        // 而且重复读之后，值仍然与金标准一致（没有被推到下一根去）
        const json& g = rows[i];
        EXPECT_BITWISE_EQ(m3.dif, from_hex(g.at("macd_dif")), tag + "重复读之后 macd.dif");
        EXPECT_BITWISE_EQ(r2, from_hex(g.at("rsi14")), tag + "重复读之后 rsi");
        EXPECT_BITWISE_EQ(k2.k, from_hex(g.at("kdj_k")), tag + "重复读之后 kdj.k");
    }
}

/*
 * ── 不同参数各占一个槽，互不干扰 ──
 * 同一根 bar 上交替读两组参数的 MACD，各自都必须与自己那组的全量重算一致。
 */
TEST(GoldenIndicatorTest, DifferentParameterSetsDoNotShareState) {
    const std::vector<Bar> bars = real_series();
    std::vector<Bar> history;
    IndicatorState istate;

    for (size_t i = 0; i < bars.size(); ++i) {
        history.push_back(bars[i]);

        StrategyContext inc;
        inc.history = &history; inc.current_bar = bars[i];
        inc.bar_index = static_cast<int>(i); inc.indicators = &istate;

        StrategyContext naive;              // 无 indicators → 全量重算
        naive.history = &history; naive.current_bar = bars[i];
        naive.bar_index = static_cast<int>(i);

        // 交替读两组参数，逼两个 slot 交错更新
        const auto a_inc = inc.macd(12, 26, 9);
        const auto b_inc = inc.macd(5, 35, 5);
        const auto a_ref = naive.macd(12, 26, 9);
        const auto b_ref = naive.macd(5, 35, 5);

        EXPECT_EQ(bits(a_inc.dif), bits(a_ref.dif)) << "bar#" << i << " (12,26,9) 组被串了";
        EXPECT_EQ(bits(b_inc.dif), bits(b_ref.dif)) << "bar#" << i << " (5,35,5) 组被串了";
    }
}

/*
 * ============================================================
 * reset() 语义：单独测，不靠别的用例顺带覆盖
 * ============================================================
 *
 * 这一组是本轮新加的。上一轮把指标改成增量递推时，递推状态散在
 * IndicatorSlot 的 c0/c1/c2/c_count/seeded 五个通用字段里，
 * **reset 这件事根本没有名字**，也就无从断言 —— 它只能间接地靠
 * 「重新构造一个 IndicatorState」来达成。
 *
 * 现在 indicators.h 里每个指标都有 reset()，可以直接调、直接比。
 * 而「用过的对象再用一次，行为必须和全新的一样」正是
 * tests/test_strategy_reset.cpp 在策略层抓到那一整类 bug 的同一个不变量。
 */

// 把一个指标喂完整条序列，返回逐根 bar 的值（位模式）
template <typename Ind, typename Get>
std::vector<std::uint64_t> feed(Ind& ind, const std::vector<Bar>& bars, Get get) {
    std::vector<std::uint64_t> out;
    out.reserve(bars.size());
    for (const auto& b : bars) {
        ind.update(b);
        out.push_back(bits(get(ind)));
    }
    return out;
}

TEST(IndicatorResetTest, ResetMakesAUsedObjectEquivalentToAFreshOne) {
    const std::vector<Bar> bars = real_series();
    const std::vector<Bar> other = flat_series();   // 先拿另一条序列把状态弄脏

    {
        MacdIndicator fresh(12, 26, 9), reused(12, 26, 9);
        const auto want = feed(fresh, bars, [](const MacdIndicator& i) { return i.value().dif; });
        feed(reused, other, [](const MacdIndicator& i) { return i.value().dif; });
        reused.reset();
        const auto got = feed(reused, bars, [](const MacdIndicator& i) { return i.value().dif; });
        EXPECT_EQ(got, want) << "MacdIndicator：reset() 之后与全新对象不等价";
        EXPECT_EQ(reused.bars_seen(), fresh.bars_seen());
    }
    {
        RsiIndicator fresh(14), reused(14);
        const auto want = feed(fresh, bars, [](const RsiIndicator& i) { return i.value(); });
        feed(reused, other, [](const RsiIndicator& i) { return i.value(); });
        reused.reset();
        const auto got = feed(reused, bars, [](const RsiIndicator& i) { return i.value(); });
        EXPECT_EQ(got, want) << "RsiIndicator：reset() 之后与全新对象不等价";
    }
    {
        auto k_of = [](const KdjIndicator& i) { return i.value().k; };
        KdjIndicator fresh(9, 3, 3);
        const auto want = feed(fresh, bars, k_of);

        /*
         * 两种脏序列长度都测：一种比 n 短（窗口半满），一种比 n 长（窗口已满）。
         *
         * ⚠️ 顺带记一条实测出来的事实，免得后人白花时间：
         *    把 `window_.clear()` 和 `head_ = 0` 从 reset() 里整个删掉，
         *    这两种情形**都还是绿的**，而且那不是测试的洞 —— 窗口是自愈的。
         *    设窗口里残留 m 个陈旧条目（m ≤ n），则随后前 n−m 次 update 走
         *    push_back 把它填满，后 m 次正好覆盖掉那 m 个陈旧槽；
         *    而第一次读取发生在第 n 次 update。也就是说陈旧数据一定在被读到
         *    之前被冲干净。
         *
         *    真正会出错的是漏清 **count_**：那会让预热期判定错位。
         *    实测注入那个缺陷，下面三个用例里有三个变红。
         *    所以这一组的判别力在载体与计数上，不在窗口上。
         */
        for (size_t dirty_len : {size_t{3}, other.size()}) {
            const std::vector<Bar> dirty(other.begin(),
                                         other.begin() + static_cast<std::ptrdiff_t>(dirty_len));
            KdjIndicator reused(9, 3, 3);
            feed(reused, dirty, k_of);
            reused.reset();
            const auto got = feed(reused, bars, k_of);
            EXPECT_EQ(got, want)
                << "KdjIndicator：先喂 " << dirty_len
                << " 根再 reset()，之后与全新对象不等价 —— "
                   "窗口与 head_ 没清干净，前 n 根读到了上一条序列的高低价";
        }
    }
}

/*
 * reset() 之后喂同一条序列，必须逐位命中金标准 —— 不只是「和全新对象一样」，
 * 而是**和改动之前的朴素实现一样**。两件事都要，少一件都不够：
 * 两个同样错的对象也能互相「等价」。
 */
TEST(IndicatorResetTest, ReusedIndicatorStillHitsTheGoldenFixture) {
    const std::vector<Bar> bars = real_series();
    const json& rows = golden().at("series").at("real_180");

    MacdIndicator macd(12, 26, 9);
    RsiIndicator rsi(14);
    KdjIndicator kdj(9, 3, 3);

    for (const auto& b : flat_series()) {        // 先弄脏
        macd.update(b); rsi.update(b); kdj.update(b);
    }
    macd.reset(); rsi.reset(); kdj.reset();

    for (size_t i = 0; i < bars.size(); ++i) {
        macd.update(bars[i]); rsi.update(bars[i]); kdj.update(bars[i]);
        const json& g = rows[i];
        const std::string tag = "reset 之后 bar#" + std::to_string(i) + " ";
        EXPECT_BITWISE_EQ(macd.value().dif, from_hex(g.at("macd_dif")), tag + "macd.dif");
        EXPECT_BITWISE_EQ(macd.value().dea, from_hex(g.at("macd_dea")), tag + "macd.dea");
        EXPECT_BITWISE_EQ(rsi.value(), from_hex(g.at("rsi14")), tag + "rsi(14)");
        EXPECT_BITWISE_EQ(kdj.value().k, from_hex(g.at("kdj_k")), tag + "kdj.k");
        EXPECT_BITWISE_EQ(kdj.value().d, from_hex(g.at("kdj_d")), tag + "kdj.d");
    }
}

/*
 * IndicatorState::reset() 把所有槽一起清干净 —— 包括 valid_size 与指纹。
 *
 * ⚠️ 只清递推载体、忘了清 valid_size 的话，下一场回测的第一次调用会看到
 *    「valid_size 正好等于 n-1」而误判成「可以步进一步」，于是拿着一个刚被
 *    清零的载体推一步就发布。那个值错得毫无征兆。这个用例专打这一点。
 */
TEST(IndicatorResetTest, IndicatorStateResetClearsPublishedMarkersToo) {
    const std::vector<Bar> bars = real_series();
    const json& rows = golden().at("series").at("real_180");

    IndicatorState st;
    std::vector<Bar> hist;

    auto run_series = [&](const std::vector<Bar>& src, bool check) {
        hist.clear();
        for (size_t i = 0; i < src.size(); ++i) {
            hist.push_back(src[i]);
            StrategyContext ctx;
            ctx.bar_index = static_cast<int>(i);
            ctx.current_bar = src[i];
            ctx.history = &hist;
            ctx.indicators = &st;
            const auto m = ctx.macd(12, 26, 9);
            const double r = ctx.rsi(14);
            const auto k = ctx.kdj(9, 3, 3);
            if (check) {
                const json& g = rows[i];
                const std::string tag = "第二场 bar#" + std::to_string(i) + " ";
                EXPECT_BITWISE_EQ(m.dif, from_hex(g.at("macd_dif")), tag + "macd.dif");
                EXPECT_BITWISE_EQ(r, from_hex(g.at("rsi14")), tag + "rsi(14)");
                EXPECT_BITWISE_EQ(k.k, from_hex(g.at("kdj_k")), tag + "kdj.k");
            }
        }
    };

    run_series(flat_series(), false);    // 第一场：把状态弄脏
    st.reset();
    run_series(bars, true);              // 第二场：必须逐位命中金标准
}

/*
 * 文档性用例：update() **不做**幂等保护，同一根 bar 喂两次就是推进两次。
 *
 * 这是刻意的分工 —— 幂等性由 StrategyContext 那层的 valid_size 提供
 * （ComboStrategy 每根 bar 会调两次 ctx.macd()）。
 * 把这条写成测试，是为了防止将来有人「顺手」在 update() 里加一道去重，
 * 那样两层各有一份判断逻辑，一旦不一致就是静默的错值。
 */
TEST(IndicatorResetTest, UpdateIsUnconditionalByDesign) {
    const std::vector<Bar> bars = real_series();

    MacdIndicator once(12, 26, 9), twice(12, 26, 9);
    for (size_t i = 0; i < 40; ++i) {
        once.update(bars[i]);
        twice.update(bars[i]);
        twice.update(bars[i]);           // 同一根喂两次
    }
    EXPECT_EQ(once.bars_seen(), 40);
    EXPECT_EQ(twice.bars_seen(), 80) << "update() 不应该自己去重 —— 幂等由门面层负责";
    EXPECT_NE(bits(once.value().dif), bits(twice.value().dif))
        << "喂两遍却得到同样的值，说明 update() 里悄悄加了去重逻辑";
}
