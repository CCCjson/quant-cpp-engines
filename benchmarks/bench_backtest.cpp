/*
 * bench_backtest.cpp — 回测引擎的进程内计时
 *
 * 为什么不直接打 HTTP 端点？
 * 因为那样测到的是「引擎 + JSON 反序列化 + HTTP 栈」的总和。
 * 想知道逐 bar 循环本身有多快，就必须绕开传输层直接构造引擎。
 * （HTTP 那条路径也测，但那是另一个口径，由 bench.py 单独跑。）
 *
 * 用法：
 *   ./bench_backtest <bars.json> <STRATEGY> <warmup> <runs>
 *
 * 输出：一行 JSON 到 stdout，供 bench.py 收集。
 *   {"strategy":"MA_CROSS","bars":250,"samples":[0.000123,...],"trades":3,"final_value":...}
 *
 * ⚠️ 结果里带上 trades / final_value，是为了让 bench.py 能核对
 *    「两个引擎确实在做同一件事」——如果 C++ 做了 3 笔交易而 Python 做了 30 笔，
 *    那速度对比就没意义了。不核对的跑分很容易在比较两个不同的工作量。
 */

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "backtest/data_loader.h"
#include "backtest/engine.h"
#include "backtest/strategy_base.h"
#include "backtest/types.h"
#include "strategies/kdj_strategy.h"
#include "strategies/ma_cross_strategy.h"
#include "strategies/macd_strategy.h"
#include "strategies/rsi_strategy.h"

using json = nlohmann::json;
using namespace backtest;

namespace {

/*
 * ============================================================
 *  对照实验：MACD 的 O(1) 增量版
 * ============================================================
 *
 * 这个类**不属于引擎**，只存在于 benchmark 里，用来回答一个问题：
 *
 *     C++ 的 MACD 回测在 25,000 根 bar 上比 Python 还慢，
 *     到底是「C++ 这个语言不行」，还是「这段 C++ 写的算法不行」？
 *
 * 引擎里的 `StrategyContext::macd()`（strategy_context.h:240）每根 bar 都
 * **从第 0 根开始重算整条 EMA 序列**，还要 new 出 4 个长度为 n 的 vector。
 * 于是单根 bar 是 O(N)，整场回测是 O(N²)。
 *
 * 下面这个版本只是把同一个递推式**增量化**：EMA 本来就是一阶递推，
 * 存住上一根的值即可，每根 bar O(1)、零堆分配。
 *
 * ⚠️ 关键：它必须产生**与原版逐位相同**的数值，否则就是在比两个不同的东西。
 *    保证来自两点：
 *      1. 递推式、系数、种子完全照抄原版
 *         （ema[0]=close[0]；dif[0]=0；dea[0]=dif[0]）
 *      2. **每根 bar 都更新状态**，包括被 guard 挡掉的前 35 根 ——
 *         原版是从第 0 根重算的，增量版漏掉任何一根都会和它错位。
 *    bench 跑完会核对 trades / final_value，对不上就是这个实验作废。
 */
class IncrementalMACDStrategy : public IStrategy {
public:
    IncrementalMACDStrategy(int fast = 12, int slow = 26, int signal = 9,
                            double position_pct = 0.95)
        : fast_(fast), slow_(slow), signal_(signal), position_pct_(position_pct) {}

    std::string name() const override { return "MACD_INCREMENTAL"; }
    std::string description() const override {
        return "对照实验：与 MACD 完全等价，但 EMA 用 O(1) 增量递推而非每 bar 重算";
    }

    std::vector<Order> on_bar(const StrategyContext& ctx) override {
        std::vector<Order> orders;

        // ── 状态更新：每根 bar 都做，绝不能跳过 ──
        const double close = ctx.current_bar.close;
        const double af = 2.0 / (fast_ + 1);
        const double as = 2.0 / (slow_ + 1);
        const double asig = 2.0 / (signal_ + 1);

        if (!seeded_) {
            ema_fast_ = close;      // 对应原版 ema_fast_arr[0] = closes[0]
            ema_slow_ = close;
            dif_ = 0.0;             // fast - slow = 0
            dea_ = dif_;            // 对应原版 dea_series[0] = dif_series[0]
            seeded_ = true;
        } else {
            ema_fast_ = af * close + (1.0 - af) * ema_fast_;
            ema_slow_ = as * close + (1.0 - as) * ema_slow_;
            dif_ = ema_fast_ - ema_slow_;
            dea_ = asig * dif_ + (1.0 - asig) * dea_;
        }

        // ── 以下与引擎里的 MACDStrategy::on_bar 完全一致 ──
        if (ctx.bar_index < slow_ + signal_) {
            return orders;
        }
        if (!initialized_) {
            prev_dif_ = dif_;
            prev_dea_ = dea_;
            initialized_ = true;
            return orders;
        }

        bool golden_cross = (prev_dif_ <= prev_dea_) && (dif_ > dea_);
        bool death_cross  = (prev_dif_ >= prev_dea_) && (dif_ < dea_);

        if (golden_cross && !ctx.has_position()) {
            double available = ctx.cash * position_pct_;
            int qty = ctx.lot_floor(available / ctx.current_bar.close);
            if (qty > 0) {
                orders.push_back(Order::market_buy(ctx.symbol, qty));
            }
        } else if (death_cross && ctx.has_position()) {
            orders.push_back(Order::market_sell(ctx.symbol, ctx.position_quantity));
        }

        prev_dif_ = dif_;
        prev_dea_ = dea_;
        return orders;
    }

private:
    int fast_, slow_, signal_;
    double position_pct_;
    double ema_fast_ = 0.0, ema_slow_ = 0.0, dif_ = 0.0, dea_ = 0.0;
    bool seeded_ = false;
    double prev_dif_ = 0.0, prev_dea_ = 0.0;
    bool initialized_ = false;
};

/*
 * ============================================================
 *  对照实验 2：保留 O(N) 重算，但消除每 bar 的堆分配
 * ============================================================
 *
 * 增量版一次改掉了两件事：① 不再每 bar 重算整条序列 ② 不再每 bar new 4 个 vector。
 * 光看它，分不清提速里有多少是 ① 的功劳、多少是 ② 的。
 *
 * 这个版本只改 ②：递推逻辑与 `ctx.macd()` 一模一样（照样从第 0 根重算），
 * 但四个缓冲区复用同一块内存，全程零分配。
 * 它与原版的差 = **堆分配单独的代价**；它与增量版的差 = **算法阶数单独的代价**。
 */
class NoAllocMACDStrategy : public IStrategy {
public:
    NoAllocMACDStrategy(int fast = 12, int slow = 26, int signal = 9,
                        double position_pct = 0.95)
        : fast_(fast), slow_(slow), signal_(signal), position_pct_(position_pct) {}

    std::string name() const override { return "MACD_NOALLOC"; }
    std::string description() const override {
        return "对照实验：仍每 bar 重算整条 EMA（O(N)），但缓冲区复用、零堆分配";
    }

    std::vector<Order> on_bar(const StrategyContext& ctx) override {
        std::vector<Order> orders;
        if (ctx.bar_index < slow_ + signal_) return orders;

        // ── 与 strategy_context.h::macd() 同样的全序列重算，只是缓冲区复用 ──
        const auto& hist = *ctx.history;
        const int n = static_cast<int>(hist.size());
        if (static_cast<int>(ef_.size()) < n) {           // 只在需要时扩容一次
            ef_.resize(n); es_.resize(n); dif_.resize(n); dea_.resize(n);
        }
        const double af = 2.0 / (fast_ + 1);
        const double as = 2.0 / (slow_ + 1);
        const double asig = 2.0 / (signal_ + 1);

        ef_[0] = hist[0].close;
        es_[0] = hist[0].close;
        for (int i = 1; i < n; ++i) {
            ef_[i] = af * hist[i].close + (1.0 - af) * ef_[i - 1];
            es_[i] = as * hist[i].close + (1.0 - as) * es_[i - 1];
        }
        for (int i = 0; i < n; ++i) dif_[i] = ef_[i] - es_[i];
        dea_[0] = dif_[0];
        for (int i = 1; i < n; ++i) dea_[i] = asig * dif_[i] + (1.0 - asig) * dea_[i - 1];

        const double dif = dif_[n - 1], dea = dea_[n - 1];

        if (!initialized_) {
            prev_dif_ = dif; prev_dea_ = dea; initialized_ = true;
            return orders;
        }
        bool golden_cross = (prev_dif_ <= prev_dea_) && (dif > dea);
        bool death_cross  = (prev_dif_ >= prev_dea_) && (dif < dea);

        if (golden_cross && !ctx.has_position()) {
            int qty = ctx.lot_floor(ctx.cash * position_pct_ / ctx.current_bar.close);
            if (qty > 0) orders.push_back(Order::market_buy(ctx.symbol, qty));
        } else if (death_cross && ctx.has_position()) {
            orders.push_back(Order::market_sell(ctx.symbol, ctx.position_quantity));
        }
        prev_dif_ = dif; prev_dea_ = dea;
        return orders;
    }

private:
    int fast_, slow_, signal_;
    double position_pct_;
    std::vector<double> ef_, es_, dif_, dea_;      // 复用，不每 bar 重新分配
    double prev_dif_ = 0.0, prev_dea_ = 0.0;
    bool initialized_ = false;
};

std::unique_ptr<IStrategy> make_strategy(const std::string& name) {
    // 参数与 Python 参照引擎的默认值一一对应（见 benchmarks/bench.py）
    if (name == "MA_CROSS") return std::make_unique<MACrossStrategy>(5, 20, 0.95);
    if (name == "MACD")     return std::make_unique<MACDStrategy>(12, 26, 9, 0.95);
    if (name == "RSI")      return std::make_unique<RSIStrategy>(14, 30.0, 70.0, 0.95);
    if (name == "KDJ")      return std::make_unique<KDJStrategy>(9, 3, 3, 20.0, 80.0, 0.95);
    if (name == "MACD_INCREMENTAL") return std::make_unique<IncrementalMACDStrategy>(12, 26, 9, 0.95);
    if (name == "MACD_NOALLOC")     return std::make_unique<NoAllocMACDStrategy>(12, 26, 9, 0.95);
    return nullptr;
}

/*
 * 跑一次完整回测。
 *
 * ⚠️ 引擎和策略都在**计时区间内**重新构造 —— 这是刻意的。
 *    策略对象带跨 bar 状态（如 MACrossStrategy::prev_fast_ma_），复用同一个实例
 *    跑第二遍，结果会和第一遍不同。而且 Python 那边每次也是新建引擎+策略，
 *    两边必须对称，否则就是在比较不同的工作量。
 */
double run_once(const std::vector<Bar>& bars, const std::string& strategy_name,
                BacktestResult* out) {
    auto t0 = std::chrono::steady_clock::now();

    BacktestEngine engine(1'000'000.0,
                          CommissionConfig::a_share(),
                          RiskConfig{},                 // 风控默认关闭，与 Python 侧对齐
                          MarketRules::a_share());
    engine.set_strategy(make_strategy(strategy_name));
    engine.load_data("TEST.SH", bars);                  // 拷贝一份，与 Python 的 data.copy() 对称
    BacktestResult result = engine.run();

    auto t1 = std::chrono::steady_clock::now();
    if (out) *out = result;
    return std::chrono::duration<double>(t1 - t0).count();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "用法: " << argv[0] << " <bars.json> <STRATEGY> [warmup] [runs]\n";
        std::cerr << "策略: MA_CROSS | MACD | RSI | KDJ | MACD_INCREMENTAL\n";
        return 1;
    }

    const std::string bars_path = argv[1];
    const std::string strategy_name = argv[2];
    const int warmup = argc > 3 ? std::atoi(argv[3]) : 3;
    const int runs   = argc > 4 ? std::atoi(argv[4]) : 10;

    if (!make_strategy(strategy_name)) {
        std::cerr << "未知策略: " << strategy_name << "\n";
        return 1;
    }

    std::ifstream fin(bars_path);
    if (!fin) {
        std::cerr << "打不开 " << bars_path << "\n";
        return 1;
    }
    json raw;
    fin >> raw;
    std::vector<Bar> bars = DataLoader::from_json(raw);
    if (bars.empty()) {
        std::cerr << "没有读到 bar\n";
        return 1;
    }

    // 预热：让分支预测器/分配器进入稳态，也把首次页错误挡在计时之外
    for (int i = 0; i < warmup; ++i) run_once(bars, strategy_name, nullptr);

    BacktestResult last;
    std::vector<double> samples;
    samples.reserve(runs);
    for (int i = 0; i < runs; ++i) samples.push_back(run_once(bars, strategy_name, &last));

    json out;
    out["engine"] = "cpp";
    out["strategy"] = strategy_name;
    out["bars"] = static_cast<int>(bars.size());
    out["samples"] = samples;
    // 用于核对两个引擎工作量一致（见文件头注释）
    out["trades"] = static_cast<int>(last.trades.size());
    out["round_trips"] = last.metrics.total_trades;
    out["final_value"] = last.metrics.final_value;

    std::cout << out.dump() << std::endl;
    return 0;
}
