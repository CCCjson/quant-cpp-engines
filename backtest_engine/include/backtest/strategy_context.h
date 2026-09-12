/*
 * strategy_context.h — 策略上下文
 *
 * StrategyContext 是回测引擎传给策略的"状态快照"。
 * 每一根 K 线（每一天）都会创建一个新的 StrategyContext，
 * 包含策略做决策需要的所有信息：
 * - 当前 K 线数据
 * - 历史 K 线（可以计算均线等指标）
 * - 账户资金和持仓情况
 *
 * 设计思想：
 * 策略不应该直接访问引擎或组合的内部状态，
 * 而是通过这个"只读快照"来获取信息。
 * 这叫做"信息隐藏"，是 OOP 的重要原则。
 *
 * 知识点：
 * - const 引用 `const std::vector<Bar>&`：
 *   引用（&）避免拷贝大量数据，const 防止策略修改数据。
 */

#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <numeric>     // std::accumulate — 用于求和
#include <algorithm>   // std::min_element, std::max_element
#include <cmath>       // std::log, std::sqrt

namespace backtest {

/*
 * ============================================================
 * 增量指标状态
 * ============================================================
 *
 * macd() / rsi() / kdj() 原来每根 bar 都从第 0 根重算整条序列 —— 对一次回测
 * 就是 O(N²)。benchmarks/ 里那次调查的核心结论就是这个：log-log 拟合斜率
 * k≈1.95，而 Python 侧因为指标是 numpy 一次性算好的，斜率 k≈1.00。
 *
 * 这个结构体承载递推所需的状态，让每根 bar 变成 O(1)（kdj 是 O(n)，n 是窗口长度）。
 *
 * ── 状态放在哪里，为什么 ──
 *
 * 放在**每个标的**的 SymbolState 里（engine.cpp），由 StrategyContext 持一个指针。
 *
 * 不能放策略对象上：组合回测下所有标的**共用同一个策略实例**
 * （engine.cpp 有明确警告），把跨 bar 状态放那里会让多标的互相串味。
 *
 * 不能放 StrategyContext 自身：它每个 (日期, 标的) 在栈上重新构造一次，
 * 放进去的状态每根 bar 都会消失。
 *
 * ── 为什么「读」必须是幂等的 ──
 *
 * 这是本设计的关键。朴素实现是**纯函数**：同一根 bar 上调用两次得到同样的值。
 * 而 ComboStrategy 会把 context 按值复制两份、对每个子策略**每根 bar 调用两次**
 * （combo_strategy.cpp）。如果改成「每次调用步进一次递推」，那里就会被步进两次 —— 错。
 *
 * 所以缓存以「这个值描述的是第几根 bar」（valid_size）为键：
 *   n == valid_size      → 直接返回已发布的值，不动递推状态（第二次调用免费且正确）
 *   n == valid_size + 1  → 且上一根 bar 指纹吻合 → 步进一次
 *   其它                  → 从头重算一遍（O(N)，只发生一次）
 *
 * ── 为什么不用指针身份判断「是不是同一条序列」 ──
 *
 * tests/test_strategies.cpp 每根 bar 把 ctx.history 指向一个**全新的子 vector**，
 * 地址每次都不同。靠指针身份会让那些测试要么每根都重算（正确但零收益），
 * 要么在栈地址被复用时给出**错误**答案。所以按内容判断：比对上一根 bar 的
 * date/close/high/low。
 *
 * ── 逐位等价 ──
 *
 * 步进执行的是与朴素循环第 i 次迭代**完全相同**的浮点运算序列，
 * 所以结果逐位相同（不是「在容差内」）。这一点由
 * tests/test_indicator_golden.cpp 的金标准守着 —— 那份 fixture 是在**改动之前**
 * 从朴素实现导出的，比较的是 double 的位模式而不是十进制。
 * 另外 CMakeLists 里的 -ffp-contract=off 保证编译器不会在某一条路径上
 * 把 a*b+c 收缩成 FMA（少一次舍入，差 1 ULP）而另一条路径不收缩。
 */
struct IndicatorSlot {
    int kind = 0;                 // 0=macd 1=rsi 2=kdj
    int p0 = 0, p1 = 0, p2 = 0;   // 参数（不同参数各占一个 slot）

    long long valid_size = -1;    // 已发布的值描述的是 history->size() 等于多少时

    // 上一根 bar 的指纹（判断是不是同一条序列的自然延长）
    std::string fp_date;
    double fp_close = 0.0, fp_high = 0.0, fp_low = 0.0;

    // 递推载体
    double c0 = 0.0, c1 = 0.0, c2 = 0.0;
    long long c_count = 0;        // rsi 用：已消费了多少个 change
    bool seeded = false;

    // 已发布的值（含预热期的哨兵）
    double v0 = 0.0, v1 = 0.0, v2 = 0.0;
};

struct IndicatorState {
    std::vector<IndicatorSlot> slots;   // 通常 ≤ 6 个，线性查找足够

    IndicatorSlot& slot_for(int kind, int p0, int p1, int p2) {
        for (auto& s : slots) {
            if (s.kind == kind && s.p0 == p0 && s.p1 == p1 && s.p2 == p2) return s;
        }
        IndicatorSlot s;
        s.kind = kind; s.p0 = p0; s.p1 = p1; s.p2 = p2;
        slots.push_back(s);
        return slots.back();
    }
};

struct StrategyContext {
    // ── 基本信息 ──
    std::string symbol;                 // 当前股票代码
    int bar_index = 0;                  // 当前是第几根 K 线（从 0 开始）

    // ── K 线数据 ──
    Bar current_bar;                    // 当前这根 K 线
    /*
     * history 包含从第一天到当前的所有 K 线。
     * history.back() 就是 current_bar。
     * 策略可以用 history 来计算均线、RSI 等技术指标。
     *
     * const 引用：策略只能读取，不能修改历史数据。
     * 为什么用 vector 而不是数组？因为每天都会增加一根 K 线，
     * vector 可以动态增长，而数组大小在编译时就固定了。
     */
    const std::vector<Bar>* history = nullptr;

    /*
     * 增量指标状态（每个标的一份，由引擎持有）。
     * 为 nullptr 时所有指标退回朴素全量重算 —— 手工构造 context 的测试、
     * 以及任何不经引擎的调用方都走这条路，行为与改动前完全一致。
     */
    IndicatorState* indicators = nullptr;

    // ── 账户状态 ──
    /*
     * ⚠️ 组合回测下 cash 是**全场共享的余额**（不是这个标的专属的钱）。
     * 同一天先被调用的标的花掉的钱，后面的标的就看不到了 —— 这正是
     * 「A 占了钱 B 就买不了」的资金竞争，逐票独立回测里不存在这回事。
     */
    double cash = 0.0;                  // 可用现金（全场共享）
    int position_quantity = 0;          // **本标的**当前持仓量（股数）
    double position_avg_price = 0.0;    // 本标的持仓平均成本价
    double total_value = 0.0;           // 总资产 = 现金 + 全部持仓市值
    double market_value = 0.0;          // 全部持仓市值（组合视角；单票时等于本标的市值）

    // ── 交易单位 ──
    /*
     * 一手股数。⛔ **别再手写 `/100)*100`** —— 那是 A 股假设，
     * 曾经被复制在 8 个策略里，对 crypto 直接失效（见 types.h::MarketRules）。
     * 一律用下面的 lot_floor()，有门禁盯着。
     */
    int lot_size = 100;

    /*
     * lot_floor — 把「按现金算出来的理论股数」向下取整到整手。
     * 不足一手返回 0（调用方据此不发单）。
     */
    int lot_floor(double raw_qty) const {
        int lot = lot_size > 0 ? lot_size : 1;
        if (!(raw_qty > 0.0)) return 0;
        long long lots = static_cast<long long>(raw_qty / lot);
        long long qty = lots * lot;
        // 单票回测里 raw_qty 可能极大（价格缩放后），钳到 int 上界防溢出
        if (qty > 2000000000LL) qty = 2000000000LL;
        return static_cast<int>(qty);
    }

    /*
     * afford — 用 weight 比例的可用现金能买多少（已取整到手）。
     * 把「available/close 再取整」这套重复了 8 次的算法收在一处。
     */
    int afford(double weight, double price) const {
        if (price <= 0.0) return 0;
        return lot_floor(cash * weight / price);
    }

    // ── 便捷方法（帮助策略快速计算常用指标） ──

    /*
     * 计算简单移动平均线（SMA）
     *
     * SMA = 过去 N 天收盘价的算术平均值
     * 例如：SMA(5) = (今天 + 昨天 + 前天 + ... + 5天前) / 5
     *
     * 均线是最基本的技术指标，用来判断趋势：
     * - 价格在均线上方 → 上升趋势
     * - 价格在均线下方 → 下降趋势
     * - 短期均线上穿长期均线（"金叉"）→ 买入信号
     * - 短期均线下穿长期均线（"死叉"）→ 卖出信号
     *
     * 参数 period：计算周期（天数）
     * 返回 0.0：如果历史数据不够长
     */
    double sma(int period) const {
        if (!history || static_cast<int>(history->size()) < period) {
            return 0.0;
        }

        double sum = 0.0;
        /*
         * 从 history 的末尾往前取 period 个元素来求和。
         * history->size() - period 是起始位置。
         *
         * size_t 是无符号整数类型，用于表示大小/索引。
         * static_cast<int> 是安全的类型转换。
         */
        for (size_t i = history->size() - period; i < history->size(); ++i) {
            sum += (*history)[i].close;
        }
        return sum / period;
    }

    /*
     * 计算过去 N 天的收益率
     *
     * 收益率 = (今天的价格 - N天前的价格) / N天前的价格
     * 例如：returns(5) 表示过去 5 天的涨跌幅
     *
     * 动量策略（Momentum Strategy）就是基于这个指标：
     * 过去涨得多的股票，未来可能继续涨（趋势延续）
     */
    double returns(int lookback) const {
        if (!history || static_cast<int>(history->size()) <= lookback) {
            return 0.0;
        }
        size_t prev_idx = history->size() - 1 - lookback;
        double prev_close = (*history)[prev_idx].close;
        if (prev_close == 0.0) return 0.0;
        return (current_bar.close - prev_close) / prev_close;
    }

    /*
     * 获取最近 N 天的最高价
     */
    double highest_close(int lookback) const {
        if (!history || history->empty()) return 0.0;
        int start = std::max(0, static_cast<int>(history->size()) - lookback);
        double max_val = 0.0;
        for (int i = start; i < static_cast<int>(history->size()); ++i) {
            max_val = std::max(max_val, (*history)[i].close);
        }
        return max_val;
    }

    /*
     * 获取最近 N 天的最低价
     */
    double lowest_close(int lookback) const {
        if (!history || history->empty()) return 1e18;
        int start = std::max(0, static_cast<int>(history->size()) - lookback);
        double min_val = 1e18;
        for (int i = start; i < static_cast<int>(history->size()); ++i) {
            min_val = std::min(min_val, (*history)[i].close);
        }
        return min_val;
    }

    /*
     * 是否有持仓
     */
    bool has_position() const {
        return position_quantity > 0;
    }

    // ── 扩展技术指标 ──

    /*
     * 指数移动平均线（EMA）
     * EMA 给近期价格更高的权重，比 SMA 更灵敏。
     * 权重因子 alpha = 2 / (period + 1)
     */
    double ema(int period) const {
        if (!history || static_cast<int>(history->size()) < period) {
            return 0.0;
        }
        double alpha = 2.0 / (period + 1);
        int n = static_cast<int>(history->size());
        // 用最早的 period 个数据的 SMA 作为初始值
        double result = 0.0;
        for (int i = 0; i < period; ++i) {
            result += (*history)[i].close;
        }
        result /= period;
        // 从 period 位置开始递推
        for (int i = period; i < n; ++i) {
            result = alpha * (*history)[i].close + (1.0 - alpha) * result;
        }
        return result;
    }

    /*
     * 计算指定数据序列在特定 offset 处的 EMA（内部辅助）
     * prices: 收盘价序列
     * period: EMA 周期
     * end_idx: 计算到哪个位置（包含）
     */
    static double ema_at(const std::vector<double>& prices, int period, int end_idx) {
        if (end_idx < 0 || period <= 0) return 0.0;
        double alpha = 2.0 / (period + 1);
        int actual_start = std::min(period, end_idx + 1);
        double result = 0.0;
        for (int i = 0; i < actual_start; ++i) {
            result += prices[i];
        }
        result /= actual_start;
        for (int i = actual_start; i <= end_idx; ++i) {
            result = alpha * prices[i] + (1.0 - alpha) * result;
        }
        return result;
    }

    /*
     * MACD 指标（三线）
     * DIF = EMA(fast) - EMA(slow)
     * DEA = EMA(DIF, signal)
     * HIST = 2 * (DIF - DEA)
     */
    struct MACDResult {
        double dif = 0.0;
        double dea = 0.0;
        double hist = 0.0;
    };

    /*
     * MACD。O(1) 每 bar（首次或序列不连续时 O(N) 重算一次）。
     *
     * 朴素版（现为 macd_full）每根 bar 重建 5 个长度 N 的 vector、重跑三遍完整
     * EMA 递推 —— 整场回测 O(N²)。这里改为持久化 (ema_fast, ema_slow, dea)
     * 三个标量，每根 bar 只推一步。
     *
     * ⚠️ 预热期也必须推。朴素版在 n < slow_period 时提前返回 {0,0,0}，
     * 但它在 n >= slow_period 时是**从第 0 根重算**的 —— 也就是说递推在概念上
     * 一直从 index 0 跑着。所以增量版在预热期同样要步进载体，只是把**发布值**
     * 压成哨兵 {0,0,0}。漏掉这一点，过了预热期的第一个值就会错。
     */
    MACDResult macd(int fast_period = 12, int slow_period = 26, int signal_period = 9) const {
        MACDResult r;
        if (!history || history->empty()) return r;
        const int n = static_cast<int>(history->size());

        if (!indicators) return macd_full(fast_period, slow_period, signal_period);

        IndicatorSlot& sl = indicators->slot_for(0, fast_period, slow_period, signal_period);

        if (sl.valid_size == n) {            // 同一根 bar 上的重复调用：原样返回
            r.dif = sl.v0; r.dea = sl.v1; r.hist = sl.v2;
            return r;
        }
        if (sl.valid_size == n - 1 && n >= 2 && matches_fp(sl, (*history)[n - 2])) {
            const double close = (*history)[n - 1].close;
            const double af = 2.0 / (fast_period + 1);
            const double as = 2.0 / (slow_period + 1);
            const double asig = 2.0 / (signal_period + 1);
            sl.c0 = af * close + (1.0 - af) * sl.c0;     // ema_fast
            sl.c1 = as * close + (1.0 - as) * sl.c1;     // ema_slow
            const double dif = sl.c0 - sl.c1;
            sl.c2 = asig * dif + (1.0 - asig) * sl.c2;   // dea
            publish_macd(sl, n, slow_period, dif, sl.c2, (*history)[n - 1]);
        } else {
            rebuild_macd(sl, fast_period, slow_period, signal_period);
        }
        r.dif = sl.v0; r.dea = sl.v1; r.hist = sl.v2;
        return r;
    }

    /*
     * RSI（相对强弱指标）
     * RSI = 100 - 100/(1 + RS)，RS = 平均涨幅 / 平均跌幅
     * RSI < 30 → 超卖（买入信号）
     * RSI > 70 → 超买（卖出信号）
     */
    /*
     * RSI（Wilder）。O(1) 每 bar。
     *
     * ⚠️ 最容易写错的地方是「累加 → 除一次 → 转递推」这三段的边界：
     * 朴素版把前 period 个 change 以**原始和**累加，到临界点**只除一次** period，
     * 之后才转 Wilder 递推。增量重写最常见的 bug 是除早了、每根都除、
     * 或递推起点差一格 —— 而它产出的是一个**看起来合理**的值（差几个百分点），
     * 不会明显出错。金标准逐根 bar 比对就是为了钉住这一段。
     *
     * 载体：c0=avg_gain（累加阶段是原始和）、c1=avg_loss、c_count=已消费的 change 数、
     *       c2=上一根收盘价。
     */
    double rsi(int period = 14) const {
        if (!history || history->empty()) return 50.0;
        const int n = static_cast<int>(history->size());

        if (!indicators) return rsi_full(period);

        IndicatorSlot& sl = indicators->slot_for(1, period, 0, 0);

        if (sl.valid_size == n) return sl.v0;

        if (sl.valid_size == n - 1 && n >= 2 && matches_fp(sl, (*history)[n - 2])) {
            const double close = (*history)[n - 1].close;
            const double ch = close - sl.c2;      // 本根 bar 的 change
            sl.c2 = close;
            step_rsi_change(sl, period, ch);
            publish_rsi(sl, n, period, (*history)[n - 1]);
        } else {
            rebuild_rsi(sl, period);
        }
        return sl.v0;
    }


    /*
     * KDJ 指标
     * RSV = (Close - LLV(N)) / (HHV(N) - LLV(N)) * 100
     * K = EMA(RSV, m1)  （实际是 SMA 平滑）
     * D = EMA(K, m2)
     * J = 3K - 2D
     */
    struct KDJResult {
        double k = 50.0;
        double d = 50.0;
        double j = 50.0;
    };

    /*
     * KDJ。从 O(N·n) 每 bar 降到 O(n) 每 bar（n 是窗口长度，默认 9）——
     * 整场从 O(N²·n) 变成 O(N·n)。
     *
     * 只持久化 (k, d) 两个标量、去掉外层那重 i 循环即可；窗口内的 HHV/LLV
     * 仍然扫这 n 根 bar。这一步是**明显逐位等价**的最小改动，也拿到了全部的
     * 复杂度阶数收益。把内层换成单调队列还能再省一个常数因子，但那是另一件事，
     * 收益小、出错面大，不在这次范围内。
     *
     * ⚠️ `(hhv == llv) ? 50.0 : ...` 这个**精确相等**判断必须原样保留。
     * 把它改成带 epsilon 的「改进」会改变回测结果 —— 金标准里的 flat 序列
     * 专门打这个分支。
     */
    KDJResult kdj(int n = 9, int m1 = 3, int m2 = 3) const {
        KDJResult r;
        if (!history || history->empty()) return r;
        const int len = static_cast<int>(history->size());

        if (!indicators) return kdj_full(n, m1, m2);

        IndicatorSlot& sl = indicators->slot_for(2, n, m1, m2);

        if (sl.valid_size == len) {
            r.k = sl.v0; r.d = sl.v1; r.j = sl.v2;
            return r;
        }
        if (sl.valid_size == len - 1 && len >= 2 && matches_fp(sl, (*history)[len - 2])) {
            if (len >= n) {
                if (!sl.seeded) { sl.c0 = 50.0; sl.c1 = 50.0; sl.seeded = true; }
                step_kdj(sl, len - 1, n, m1, m2);
            }
            publish_kdj(sl, len, n, (*history)[len - 1]);
        } else {
            rebuild_kdj(sl, n, m1, m2);
        }
        r.k = sl.v0; r.d = sl.v1; r.j = sl.v2;
        return r;
    }


    /*
     * 布林带（Bollinger Bands）
     * 中轨 = SMA(period)
     * 上轨 = 中轨 + num_std × 标准差
     * 下轨 = 中轨 - num_std × 标准差
     */
    struct BollingerResult {
        double upper = 0.0;
        double middle = 0.0;
        double lower = 0.0;
    };

    BollingerResult bollinger(int period = 20, double num_std = 2.0) const {
        BollingerResult r;
        r.middle = sma(period);
        if (r.middle == 0.0) return r;
        double sd = stddev(period);
        r.upper = r.middle + num_std * sd;
        r.lower = r.middle - num_std * sd;
        return r;
    }

    /*
     * 标准差
     * 衡量价格波动的离散程度
     */
    double stddev(int period) const {
        if (!history || static_cast<int>(history->size()) < period) {
            return 0.0;
        }
        double mean = sma(period);
        double sum_sq = 0.0;
        for (size_t i = history->size() - period; i < history->size(); ++i) {
            double diff = (*history)[i].close - mean;
            sum_sq += diff * diff;
        }
        return std::sqrt(sum_sq / period);
    }

private:
    // ────────────────────────────────────────────────────────
    // 增量指标的内部实现
    //
    // *_full() 就是改动前的朴素全量重算，**原样保留并保持可达**：
    //   - indicators == nullptr 时（手工构造 context 的测试等）直接走它；
    //   - 序列不连续时由 rebuild_* 调用它来重建载体。
    // 保留它还有一个好处：金标准测试可以在同一个进程里比对
    // 「朴素 vs 增量」，而不只是比对一份离线 fixture。
    // ────────────────────────────────────────────────────────

    bool matches_fp(const IndicatorSlot& sl, const Bar& prev) const {
        return sl.fp_date == prev.date && sl.fp_close == prev.close
            && sl.fp_high == prev.high && sl.fp_low == prev.low;
    }
    static void stamp_fp(IndicatorSlot& sl, long long n, const Bar& last) {
        sl.valid_size = n;
        sl.fp_date = last.date; sl.fp_close = last.close;
        sl.fp_high = last.high; sl.fp_low = last.low;
    }

    // ── MACD ──
    MACDResult macd_full(int fast_period, int slow_period, int signal_period) const {
        MACDResult r;
        if (!history || static_cast<int>(history->size()) < slow_period) return r;
        const int n = static_cast<int>(history->size());
        const double af = 2.0 / (fast_period + 1);
        const double as = 2.0 / (slow_period + 1);
        const double asig = 2.0 / (signal_period + 1);
        double ef = (*history)[0].close, es = (*history)[0].close;
        double dif = ef - es;
        double dea = dif;
        for (int i = 1; i < n; ++i) {
            const double c = (*history)[i].close;
            ef = af * c + (1.0 - af) * ef;
            es = as * c + (1.0 - as) * es;
            dif = ef - es;
            dea = asig * dif + (1.0 - asig) * dea;
        }
        r.dif = dif; r.dea = dea; r.hist = 2.0 * (r.dif - r.dea);
        return r;
    }
    static void publish_macd(IndicatorSlot& sl, int n, int slow_period,
                             double dif, double dea, const Bar& last) {
        if (n < slow_period) { sl.v0 = 0.0; sl.v1 = 0.0; sl.v2 = 0.0; }
        else { sl.v0 = dif; sl.v1 = dea; sl.v2 = 2.0 * (dif - dea); }
        stamp_fp(sl, n, last);
    }
    void rebuild_macd(IndicatorSlot& sl, int fast_period, int slow_period,
                      int signal_period) const {
        const int n = static_cast<int>(history->size());
        const double af = 2.0 / (fast_period + 1);
        const double as = 2.0 / (slow_period + 1);
        const double asig = 2.0 / (signal_period + 1);
        double ef = (*history)[0].close, es = (*history)[0].close;
        double dif = ef - es;
        double dea = dif;
        for (int i = 1; i < n; ++i) {
            const double c = (*history)[i].close;
            ef = af * c + (1.0 - af) * ef;
            es = as * c + (1.0 - as) * es;
            dif = ef - es;
            dea = asig * dif + (1.0 - asig) * dea;
        }
        sl.c0 = ef; sl.c1 = es; sl.c2 = dea; sl.seeded = true;
        publish_macd(sl, n, slow_period, dif, dea, (*history)[n - 1]);
    }

    // ── RSI ──
    double rsi_full(int period) const {
        if (!history || static_cast<int>(history->size()) < period + 1) return 50.0;
        const int n = static_cast<int>(history->size());
        double avg_gain = 0.0, avg_loss = 0.0;
        int seen = 0;
        for (int i = 1; i < n; ++i) {
            const double ch = (*history)[i].close - (*history)[i - 1].close;
            ++seen;
            if (seen <= period) {                       // 阶段一：原始和
                if (ch > 0) avg_gain += ch; else avg_loss += (-ch);
                if (seen == period) { avg_gain /= period; avg_loss /= period; }
            } else {                                    // 阶段二：Wilder 递推
                const double g = ch > 0 ? ch : 0.0;
                const double l = ch < 0 ? -ch : 0.0;
                avg_gain = (avg_gain * (period - 1) + g) / period;
                avg_loss = (avg_loss * (period - 1) + l) / period;
            }
        }
        if (avg_loss == 0.0) return 100.0;
        return 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
    }
    static void step_rsi_change(IndicatorSlot& sl, int period, double ch) {
        ++sl.c_count;
        if (sl.c_count <= period) {
            if (ch > 0) sl.c0 += ch; else sl.c1 += (-ch);
            if (sl.c_count == period) { sl.c0 /= period; sl.c1 /= period; }
        } else {
            const double g = ch > 0 ? ch : 0.0;
            const double l = ch < 0 ? -ch : 0.0;
            sl.c0 = (sl.c0 * (period - 1) + g) / period;
            sl.c1 = (sl.c1 * (period - 1) + l) / period;
        }
    }
    static void publish_rsi(IndicatorSlot& sl, int n, int period, const Bar& last) {
        if (n < period + 1) sl.v0 = 50.0;
        else if (sl.c1 == 0.0) sl.v0 = 100.0;
        else sl.v0 = 100.0 - 100.0 / (1.0 + sl.c0 / sl.c1);
        stamp_fp(sl, n, last);
    }
    void rebuild_rsi(IndicatorSlot& sl, int period) const {
        const int n = static_cast<int>(history->size());
        sl.c0 = 0.0; sl.c1 = 0.0; sl.c_count = 0;
        for (int i = 1; i < n; ++i) {
            step_rsi_change(sl, period, (*history)[i].close - (*history)[i - 1].close);
        }
        sl.c2 = (*history)[n - 1].close;    // 上一根收盘价，供下次步进用
        sl.seeded = true;
        publish_rsi(sl, n, period, (*history)[n - 1]);
    }

    // ── KDJ ──
    KDJResult kdj_full(int n, int m1, int m2) const {
        KDJResult r;
        if (!history || static_cast<int>(history->size()) < n) return r;
        const int len = static_cast<int>(history->size());
        double k_val = 50.0, d_val = 50.0;
        for (int i = n - 1; i < len; ++i) {
            double hhv = (*history)[i].high, llv = (*history)[i].low;
            for (int j = i - n + 1; j < i; ++j) {
                hhv = std::max(hhv, (*history)[j].high);
                llv = std::min(llv, (*history)[j].low);
            }
            const double rsv = (hhv == llv) ? 50.0
                : ((*history)[i].close - llv) / (hhv - llv) * 100.0;
            k_val = (rsv + (m1 - 1) * k_val) / m1;
            d_val = (k_val + (m2 - 1) * d_val) / m2;
        }
        r.k = k_val; r.d = d_val; r.j = 3.0 * k_val - 2.0 * d_val;
        return r;
    }
    // 推进一根 bar（i 是该 bar 在 history 中的下标）
    void step_kdj(IndicatorSlot& sl, int i, int n, int m1, int m2) const {
        double hhv = (*history)[i].high, llv = (*history)[i].low;
        for (int j = i - n + 1; j < i; ++j) {
            hhv = std::max(hhv, (*history)[j].high);
            llv = std::min(llv, (*history)[j].low);
        }
        const double rsv = (hhv == llv) ? 50.0
            : ((*history)[i].close - llv) / (hhv - llv) * 100.0;
        sl.c0 = (rsv + (m1 - 1) * sl.c0) / m1;
        sl.c1 = (sl.c0 + (m2 - 1) * sl.c1) / m2;
    }
    static void publish_kdj(IndicatorSlot& sl, int len, int n, const Bar& last) {
        if (len < n) { sl.v0 = 50.0; sl.v1 = 50.0; sl.v2 = 50.0; }
        else { sl.v0 = sl.c0; sl.v1 = sl.c1; sl.v2 = 3.0 * sl.c0 - 2.0 * sl.c1; }
        stamp_fp(sl, len, last);
    }
    void rebuild_kdj(IndicatorSlot& sl, int n, int m1, int m2) const {
        const int len = static_cast<int>(history->size());
        sl.c0 = 50.0; sl.c1 = 50.0; sl.seeded = true;
        for (int i = n - 1; i < len; ++i) step_kdj(sl, i, n, m1, m2);
        publish_kdj(sl, len, n, (*history)[len - 1]);
    }

public:
};

}  // namespace backtest
