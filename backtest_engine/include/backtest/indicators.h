/*
 * indicators.h — 有状态指标：update(bar) 推进一步，reset() 清回初值
 *
 * ============================================================
 * 这个文件是什么，不是什么
 * ============================================================
 *
 * **不是**一次重新实现。下面每个 update() 里的递推式、系数、种子，都是从
 * strategy_context.h 里原样搬过来的，一个字符没改。这是硬要求：
 * 指标金标准 fixture（tests/data/indicator_golden.json）是在增量化**之前**
 * 从朴素实现导出的，逐根 bar 记录 double 的位模式。搬运只要改了一处运算顺序，
 * 那份 fixture 就会红。
 *
 * 搬过来是为了让**生命周期变成显式的**：
 *
 *   改之前：递推状态散在 IndicatorSlot 的 c0/c1/c2/c_count/seeded 五个通用字段里，
 *           三个指标共用同一组字段，各自赋予不同含义（c2 在 macd 里是 dea，
 *           在 rsi 里是「上一根收盘价」）。能跑，但读代码的人必须记住这张对照表，
 *           而且 reset 语义**没有名字**，也就无从测试。
 *   改之后：一个指标一个类型，字段各有其名，reset() 是可以直接调、直接断言的东西。
 *
 * tests/test_strategy_reset.cpp 里那类「跑两遍必须等于跑一遍」的 bug，
 * 根源就是 reset 语义没有落成一等公民。指标这边先把它落实。
 *
 * ============================================================
 * 为什么 update() 不负责判断「该不该推进」
 * ============================================================
 *
 * update() 就是**无条件推进一步**，不做任何幂等保护。
 *
 * 判断「这根 bar 是不是新的」是 StrategyContext 那层的事（它按
 * valid_size + 上一根 bar 的指纹判断）。原因是 ComboStrategy 每根 bar 会对
 * 每个子策略**调用两次** ctx.macd()，如果 update() 自己去猜「我是不是已经
 * 推过这根了」，就得在这里重复一份同样的判断逻辑，而且两处一旦不一致就是
 * 静默的错值。
 *
 * 所以职责切干净：**这一层只管算，那一层只管什么时候算。**
 *
 * ============================================================
 * 预热期为什么也要推
 * ============================================================
 *
 * 朴素实现在数据不足时提前返回哨兵值（macd 返回 {0,0,0}、rsi 返回 50、
 * kdj 返回 {50,50,50}），但它在数据够了之后是**从第 0 根重算**的 ——
 * 也就是说递推在概念上一直从 index 0 跑着。
 *
 * 所以 update() 在预热期同样要推进载体，只是 value() 把**发布值**压成哨兵。
 * 漏掉这一点，过了预热期的第一个值就会错，而且错得很像对的。
 */

#pragma once

#include "types.h"

#include <algorithm>
#include <vector>

namespace backtest {

/*
 * 所有有状态指标的公共契约。
 *
 * 只有两个动作：喂一根 bar，或者清回初值。取值由各自的 value() 给出 ——
 * 不放进接口里，是因为 macd 返回三个分量、rsi 返回一个，硬凑一个公共返回类型
 * 只会逼出一个谁都不合身的结构体。接口负责**生命周期**，取值各管各的。
 */
class IIncrementalIndicator {
public:
    virtual ~IIncrementalIndicator() = default;

    /* 推进一根 bar。无条件推进，不做幂等保护（理由见文件头）。 */
    virtual void update(const Bar& bar) = 0;

    /* 清回「一根 bar 都没见过」的状态。必须与全新构造的对象完全等价。 */
    virtual void reset() = 0;

    /* 已经喂进来多少根 bar —— 预热期判定要用。 */
    virtual long long bars_seen() const = 0;
};

/* ──────────────────────────────────────────────────────────
 * MACD
 * ────────────────────────────────────────────────────────── */

struct MacdValue {
    double dif = 0.0;
    double dea = 0.0;
    double hist = 0.0;
};

class MacdIndicator final : public IIncrementalIndicator {
public:
    MacdIndicator(int fast, int slow, int signal)
        : fast_(fast), slow_(slow), signal_(signal) {}

    void update(const Bar& bar) override {
        const double c = bar.close;
        if (count_ == 0) {
            // 对应朴素实现的种子：ef[0] = es[0] = closes[0]；dif[0] = 0；dea[0] = dif[0]
            ema_fast_ = c;
            ema_slow_ = c;
            dif_ = ema_fast_ - ema_slow_;
            dea_ = dif_;
        } else {
            const double af = 2.0 / (fast_ + 1);
            const double as = 2.0 / (slow_ + 1);
            const double asig = 2.0 / (signal_ + 1);
            ema_fast_ = af * c + (1.0 - af) * ema_fast_;
            ema_slow_ = as * c + (1.0 - as) * ema_slow_;
            dif_ = ema_fast_ - ema_slow_;
            dea_ = asig * dif_ + (1.0 - asig) * dea_;
        }
        ++count_;
    }

    void reset() override {
        ema_fast_ = ema_slow_ = dif_ = dea_ = 0.0;
        count_ = 0;
    }

    long long bars_seen() const override { return count_; }

    MacdValue value() const {
        MacdValue v;
        if (count_ < slow_) return v;            // 预热期哨兵 {0,0,0}
        v.dif = dif_;
        v.dea = dea_;
        v.hist = 2.0 * (dif_ - dea_);
        return v;
    }

private:
    int fast_, slow_, signal_;
    double ema_fast_ = 0.0, ema_slow_ = 0.0, dif_ = 0.0, dea_ = 0.0;
    long long count_ = 0;
};

/* ──────────────────────────────────────────────────────────
 * RSI（Wilder）
 * ────────────────────────────────────────────────────────── */

class RsiIndicator final : public IIncrementalIndicator {
public:
    explicit RsiIndicator(int period) : period_(period) {}

    /*
     * ⚠️ 最容易写错的是「累加 → 除一次 → 转递推」这三段的边界：
     *    前 period 个 change 以**原始和**累加，到临界点**只除一次** period，
     *    之后才转 Wilder 递推。除早了、每根都除、或递推起点差一格，
     *    产出的都是一个**看起来合理**的值（差几个百分点），不会明显出错。
     *    金标准逐根 bar 比对就是为了钉住这一段。
     */
    void update(const Bar& bar) override {
        if (count_ == 0) {
            prev_close_ = bar.close;             // 第一根没有 change
            ++count_;
            return;
        }
        const double ch = bar.close - prev_close_;
        prev_close_ = bar.close;
        ++changes_;
        if (changes_ <= period_) {               // 阶段一：原始和
            if (ch > 0) avg_gain_ += ch; else avg_loss_ += (-ch);
            if (changes_ == period_) { avg_gain_ /= period_; avg_loss_ /= period_; }
        } else {                                 // 阶段二：Wilder 递推
            const double g = ch > 0 ? ch : 0.0;
            const double l = ch < 0 ? -ch : 0.0;
            avg_gain_ = (avg_gain_ * (period_ - 1) + g) / period_;
            avg_loss_ = (avg_loss_ * (period_ - 1) + l) / period_;
        }
        ++count_;
    }

    void reset() override {
        avg_gain_ = avg_loss_ = prev_close_ = 0.0;
        changes_ = 0;
        count_ = 0;
    }

    long long bars_seen() const override { return count_; }

    double value() const {
        if (count_ < period_ + 1) return 50.0;   // 数据不足报中性值
        if (avg_loss_ == 0.0) return 100.0;
        return 100.0 - 100.0 / (1.0 + avg_gain_ / avg_loss_);
    }

private:
    int period_;
    double avg_gain_ = 0.0, avg_loss_ = 0.0, prev_close_ = 0.0;
    long long changes_ = 0;      // 已消费的 change 数（不是 bar 数）
    long long count_ = 0;
};

/* ──────────────────────────────────────────────────────────
 * KDJ
 * ────────────────────────────────────────────────────────── */

struct KdjValue {
    double k = 50.0;
    double d = 50.0;
    double j = 50.0;
};

class KdjIndicator final : public IIncrementalIndicator {
public:
    KdjIndicator(int n, int m1, int m2) : n_(n), m1_(m1), m2_(m2) {
        window_.reserve(static_cast<size_t>(n > 0 ? n : 1));
    }

    /*
     * 窗口内的 HHV/LLV 仍然扫这 n 根 bar，没有换成单调队列 —— 那是刻意的。
     * 第四轮的剖析显示 KDJ 的指标整体只占整场回测的 3.7%，
     * 换单调队列的收益进不了测量精度，而预注册里写明了「< 10% 则不做」。
     * 见 benchmarks/README.md 的第四轮。
     *
     * ⚠️ `(hhv == llv) ? 50.0 : ...` 这个**精确相等**判断必须原样保留。
     *    改成带 epsilon 的「改进」会改变回测结果 —— 金标准里的 flat 序列
     *    专门打这个分支。
     */
    void update(const Bar& bar) override {
        /*
         * 窗口存的是 {high, low} 两个 double，不是整个 Bar，而且用环形缓冲
         * 而不是 erase(begin())。
         *
         * 第一版图省事写成 `std::vector<Bar>` + `erase(begin())`：每根 bar 要
         * 搬 n-1 个 Bar，而 Bar 里有个 std::string date —— 等于每根 bar 做
         * 8 次字符串拷贝。实测 KDJ @25,000 根比重构前慢 4.5%，是这一处引入的。
         * 换成下面这样之后回到噪声里。
         *
         * ⚠️ 扫描顺序变了（环形缓冲不再是时间序），但这**不影响逐位等价**：
         *    max/min 是精确运算，不产生舍入，换顺序结果完全相同。
         *    参与 rsv 那个除法的 hhv/llv 是同一对值。金标准盯着这一点。
         */
        if (static_cast<int>(window_.size()) < n_) {
            window_.push_back({bar.high, bar.low});
        } else {
            window_[head_] = {bar.high, bar.low};
            head_ = (head_ + 1) % n_;
        }
        ++count_;

        if (count_ < n_) return;                 // 预热期：载体还不动
        if (!seeded_) { k_ = 50.0; d_ = 50.0; seeded_ = true; }

        double hhv = bar.high, llv = bar.low;
        for (const HighLow& w : window_) {
            hhv = std::max(hhv, w.high);
            llv = std::min(llv, w.low);
        }
        const double rsv = (hhv == llv) ? 50.0
            : (bar.close - llv) / (hhv - llv) * 100.0;
        k_ = (rsv + (m1_ - 1) * k_) / m1_;
        d_ = (k_ + (m2_ - 1) * d_) / m2_;
    }

    void reset() override {
        window_.clear();
        head_ = 0;
        k_ = 50.0;
        d_ = 50.0;
        seeded_ = false;
        count_ = 0;
    }

    long long bars_seen() const override { return count_; }

    KdjValue value() const {
        KdjValue v;
        if (count_ < n_) return v;               // 预热期哨兵 {50,50,50}
        v.k = k_;
        v.d = d_;
        v.j = 3.0 * k_ - 2.0 * d_;
        return v;
    }

private:
    struct HighLow { double high, low; };

    int n_, m1_, m2_;
    std::vector<HighLow> window_;   // 最近 n 根的高低价（环形），供 HHV/LLV 扫描
    int head_ = 0;                  // 环形缓冲里下一个要覆盖的位置
    double k_ = 50.0, d_ = 50.0;
    bool seeded_ = false;
    long long count_ = 0;
};

}  // namespace backtest
