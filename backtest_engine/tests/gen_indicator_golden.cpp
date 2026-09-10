/*
 * gen_indicator_golden.cpp — 生成指标金标准 fixture
 *
 * ============================================================
 * 这个文件存在的理由
 * ============================================================
 *
 * 本仓库的 benchmarks/ 里有一条核心结论：引擎里的 macd()/rsi()/kdj() 每根 bar
 * 都从头重算整条序列，是 O(N²)；改成增量递推能提速 277×，而且**逐位等价**。
 *
 * 「逐位等价」是这条结论的全部分量所在。如果只是「差不多相等」，那这就是一次
 * 普通的性能优化，还附带一个「回测数字变了」的风险；只有逐位相同，才能说
 * 「算的是同一件事，只是算得快了」。
 *
 * 问题是：**当前没有任何测试断言过任何指标的值。**
 * macd / rsi / kdj / ema / bollinger / stddev 全都是零值断言，
 * tests/ 里只有 sma() 和 returns() 有测试。也就是说，那个逐位等价的说法
 * 目前没有任何东西守着——重写之后拿什么证明它没变？
 *
 * 所以顺序必须是：
 *   1. 先从**现在这份朴素实现**导出每根 bar 的指标值，作为金标准提交进仓库；
 *   2. 之后才允许改实现；
 *   3. 改完用金标准证明逐位相同。
 *
 * 反过来做（先改再抓）等于用新实现给自己出考卷，什么也证明不了。
 *
 * ============================================================
 * 为什么存十六进制位模式而不是小数
 * ============================================================
 *
 * 浮点数转成十进制字符串再转回来，是否无损取决于精度位数、格式化实现和
 * locale。要断言的既然是「逐位相同」，那就直接存 double 的 8 字节位模式
 * （memcpy 到 uint64_t，按 %016llx 输出）。这样：
 *   - 完全无歧义，不受序列化实现影响；
 *   - 比较时也不用把它变回 double，直接比整数；
 *   - ±0.0 能区分，NaN 也能按位比较。
 *
 * 另外同时输出一个 *_approx 十进制值，纯粹给人看，不参与任何断言。
 *
 * ============================================================
 * 为什么每根 bar 都存，而不只存最后一根
 * ============================================================
 *
 * 因为最容易写错的地方是**预热窗口和它的过渡 bar**：
 *   - rsi() 的前 period 个变化是「累加原始和、到临界点除一次、之后才转递推」
 *     （strategy_context.h:303-314）。增量重写时最可能的 bug 就是除早了、
 *     每根都除、或者递推起点差一格——而它产生的是一个**看起来合理**的值，
 *     差几个百分点，不会明显出错。
 *   - macd() 在 n < slow_period 时报 {0,0,0}，但递推状态其实从 index 0 就在推。
 *   - kdj() 在 n < n_ 时报 {50,50,50}。
 *
 * 只比最后一根 bar，这些全都漏。逐根比才钉得住。
 *
 * 用法（一次性生成，产物提交进仓库）：
 *   ./backtest_engine/build/indicator_golden_gen > backtest_engine/tests/data/indicator_golden.json
 */

#include "backtest/strategy_context.h"
#include "backtest/data_loader.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace backtest;
using nlohmann::json;

// double → 8 字节位模式的十六进制表示。
// 用 memcpy 而不是 reinterpret_cast<uint64_t*>：后者是严格别名违规（UB），
// 而且这个项目现在开了 UBSan，会被抓。
static std::string bits_hex(double d) {
    std::uint64_t u = 0;
    std::memcpy(&u, &d, sizeof u);
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(u));
    return buf;
}

// 造一根 bar。open/high/low/close 全给齐，因为 kdj 要用 high/low。
static Bar mk(const std::string& date, double open, double high, double low, double close) {
    Bar b;
    b.date = date;
    b.open = open;
    b.high = high;
    b.low = low;
    b.close = close;
    b.volume = 1000000.0;
    return b;
}

static std::string day(int i) {
    char buf[16];
    // 每月按 28 天算，保证日期严格递增且可比（引擎按字符串比日期）
    std::snprintf(buf, sizeof buf, "2025-%02d-%02d", (i / 28) + 1, (i % 28) + 1);
    return buf;
}

/*
 * 逐根 bar 算指标。
 *
 * 关键：ctx.history 指向一个**逐步增长**的 vector，模拟引擎里的真实情形
 * （engine.cpp:354-374 先 push_back 今天的 bar，再构造 context）。
 * 指标的种子取自 history[0]，所以数值依赖「回测从哪一根 bar 开始」——
 * 这个性质必须被金标准一起钉住，否则将来的重写可能悄悄改掉它。
 */
static json series_golden(const std::string& name, const std::vector<Bar>& bars) {
    json rows = json::array();
    std::vector<Bar> history;
    history.reserve(bars.size());

    for (size_t i = 0; i < bars.size(); ++i) {
        history.push_back(bars[i]);

        StrategyContext ctx;
        ctx.symbol = name;
        ctx.bar_index = static_cast<int>(i);
        ctx.current_bar = bars[i];
        ctx.history = &history;

        const auto m = ctx.macd(12, 26, 9);
        const double r = ctx.rsi(14);
        const auto k = ctx.kdj(9, 3, 3);
        const auto bb = ctx.bollinger(20, 2.0);
        const double s5 = ctx.sma(5);
        const double s20 = ctx.sma(20);
        const double sd = ctx.stddev(20);

        rows.push_back({
            {"i", i},
            {"date", bars[i].date},
            // ── 位模式：唯一参与断言的东西 ──
            {"macd_dif", bits_hex(m.dif)},
            {"macd_dea", bits_hex(m.dea)},
            {"macd_hist", bits_hex(m.hist)},
            {"rsi14", bits_hex(r)},
            {"kdj_k", bits_hex(k.k)},
            {"kdj_d", bits_hex(k.d)},
            {"kdj_j", bits_hex(k.j)},
            {"boll_upper", bits_hex(bb.upper)},
            {"boll_middle", bits_hex(bb.middle)},
            {"boll_lower", bits_hex(bb.lower)},
            {"sma5", bits_hex(s5)},
            {"sma20", bits_hex(s20)},
            {"stddev20", bits_hex(sd)},
            // ── 给人看的十进制，任何断言都不碰它 ──
            {"_approx", {{"dif", m.dif}, {"dea", m.dea}, {"rsi", r}, {"k", k.k}, {"d", k.d}}},
        });
    }
    return rows;
}

int main(int argc, char** argv) {
    // 真实日线 fixture 的路径由构建系统注入（见 CMakeLists 的 GOLDEN_BARS_PATH）
    std::string bars_path = (argc > 1) ? argv[1] : std::string(GOLDEN_BARS_PATH);

    std::ifstream in(bars_path);
    if (!in) {
        std::cerr << "打不开真实日线 fixture: " << bars_path << "\n";
        return 1;
    }
    json raw;
    in >> raw;
    std::vector<Bar> real_bars = DataLoader::from_json(raw);
    if (real_bars.empty()) {
        std::cerr << "fixture 里一根 bar 都没有: " << bars_path << "\n";
        return 1;
    }

    /*
     * 三条构造序列，专门打那些「重写时最容易悄悄改掉」的分支：
     *
     *   flat    —— high == low == close，逼出 kdj() 里 `(hhv == llv) ? 50.0 : ...`
     *              那个**精确相等**判断（strategy_context.h:350）。任何把它改成
     *              带 epsilon 的「改进」都会在这里变红，这正是要的效果。
     *   rising  —— 单调上涨，逼出 rsi() 的 `avg_loss == 0.0 → return 100.0`
     *              （strategy_context.h:316）。
     *   falling —— 单调下跌，逼出 avg_gain 为 0 的另一侧。
     */
    std::vector<Bar> flat, rising, falling;
    for (int i = 0; i < 60; ++i) {
        flat.push_back(mk(day(i), 50.0, 50.0, 50.0, 50.0));
        double up = 100.0 + i;
        rising.push_back(mk(day(i), up, up + 0.5, up - 0.5, up));
        double dn = 200.0 - i;
        falling.push_back(mk(day(i), dn, dn + 0.5, dn - 0.5, dn));
    }

    json out;
    out["_README"] =
        "指标金标准。所有指标字段是 double 的 8 字节位模式（十六进制），"
        "断言必须逐位比较，不要用 EXPECT_DOUBLE_EQ（它容许 4 ULP，"
        "正好放过要防的那类误差）。_approx 仅供人读，不参与断言。"
        "由 tests/gen_indicator_golden.cpp 生成，不要手改。";
    out["params"] = {
        {"macd", {{"fast", 12}, {"slow", 26}, {"signal", 9}}},
        {"rsi", {{"period", 14}}},
        {"kdj", {{"n", 9}, {"m1", 3}, {"m2", 3}}},
        {"bollinger", {{"period", 20}, {"num_std", 2.0}}},
        {"sma", {5, 20}},
        {"stddev", {{"period", 20}}},
    };
    out["series"] = {
        {"real_180", series_golden("REAL", real_bars)},
        {"flat_60", series_golden("FLAT", flat)},
        {"rising_60", series_golden("RISING", rising)},
        {"falling_60", series_golden("FALLING", falling)},
    };

    std::cout << out.dump(1) << std::endl;
    return 0;
}
