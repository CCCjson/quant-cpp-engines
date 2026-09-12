/*
 * bench_orderbook.cpp — 订单簿的进程内性能基线
 *
 * 这里没有 Python 对照组，给的是**绝对数字**。
 *
 * 订单簿这类系统，均值是没有意义的指标 —— 一个 p50 很漂亮但 p99 爆掉的撮合引擎
 * 在真实盘口里就是灾难。所以下面所有延迟都报分位数（p50/p90/p99/p999/max），
 * 吞吐单独报。
 *
 * ⚠️ 时钟分辨率的坑：
 *    macOS 的 steady_clock 底层是 mach_absolute_time，分辨率约 41.67ns（24MHz）。
 *    单次 get_depth 只有 ~250ns，也就是**只有 6 个 tick**，量化误差 ±17%。
 *    所以每个用例同时给两个数：
 *      · latency  —— 逐次计时的分位数，能看尾部，但被量化到 41.67ns 的整数倍
 *      · amortized_ns —— 整批总耗时 ÷ 次数，不受量化影响，但看不到尾部
 *    两个一起看才完整：分位数判尾部风险，摊销值判真实平均成本。
 *
 * 用法：
 *   ./bench_orderbook [runs_per_case]
 * 输出：一行 JSON 到 stdout。
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "orderbook/limit_order_book.h"
#include "orderbook/session.h"
#include "orderbook/price.h"
#include "orderbook/types.h"

using json = nlohmann::json;
using namespace orderbook;
using Clock = std::chrono::steady_clock;

namespace {

/*
 * 确定性伪随机（线性同余）。
 * ⛔ 不用 std::mt19937 + random_device：benchmark 必须可复现，
 *    每次跑不同的订单流，数字就没法横向比。
 */
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint64_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return s >> 33; }
    int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint64_t>(hi - lo + 1)); }
};

struct Percentiles {
    double p50, p90, p99, p999, max, mean;
};

Percentiles percentiles(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        if (v.empty()) return 0.0;
        size_t i = static_cast<size_t>(q * (v.size() - 1));
        return v[i];
    };
    double sum = 0.0;
    for (double x : v) sum += x;
    return {at(0.50), at(0.90), at(0.99), at(0.999), v.empty() ? 0.0 : v.back(),
            v.empty() ? 0.0 : sum / v.size()};
}

/*
 * 时钟粒度实测。
 *
 * 为什么不写死 41.67ns：那是 macOS/24MHz 的值，Linux 上通常是 1ns。
 * CI 现在两个平台都跑，写死会让另一个平台的可信度标注完全错掉。
 * 实测办法：连续取时间戳，记下**最小的非零差值**，那就是这台机器上
 * 时钟能分辨的最小单位。
 */
double clock_granularity_ns(int n = 200000) {
    double best = 1e18;
    for (int i = 0; i < n; ++i) {
        auto a = Clock::now();
        auto b = Clock::now();
        double d = std::chrono::duration<double, std::nano>(b - a).count();
        if (d > 0.0 && d < best) best = d;
    }
    return (best > 1e17) ? 0.0 : best;
}

json pct_json(Percentiles p, double granularity_ns) {
    json j = {{"p50_ns", p.p50}, {"p90_ns", p.p90}, {"p99_ns", p.p99},
              {"p999_ns", p.p999}, {"max_ns", p.max}, {"mean_ns", p.mean}};
    /*
     * 可信度标注。
     *
     * 逐次计时的值被量化到时钟粒度的整数倍。如果 p50 只有粒度的几倍，
     * 那它的尾数就是量化产物而不是真实信号 —— 比如 macOS 上 get_depth 的
     * p50 报 250ns，其实是「6 个 tick」，真实值在 229~271 之间。
     *
     * 这一点原来只写在文件头的注释里，结果 JSON 里没有任何痕迹，
     * 于是生成的 README 会把 250/292/583 这种量化产物当成精确数字展示。
     * 现在把判据连同粒度一起写进结果，让读数的人能自己判断。
     */
    j["clock_granularity_ns"] = granularity_ns;
    if (granularity_ns > 0.0) {
        const double ticks = p.p50 / granularity_ns;
        j["p50_clock_ticks"] = ticks;
        /*
         * 阈值取 10 个 tick，理由是可算出来的而不是拍的：
         * 单次测量的量化误差是 ±0.5 个 tick，相对误差 = 0.5 / ticks。
         * ticks = 10 对应 ±5%，这是「还能当数字看」的边界；
         * ticks = 3（本机 get_depth 的实际情形）对应 ±17%，
         * 那时候 p50 的十位数已经没有意义了。
         */
        const double rel_err = 0.5 / ticks;
        j["p50_quantization_rel_err"] = rel_err;
        j["percentiles_quantization_limited"] = (ticks < 10.0);
        if (ticks < 10.0) {
            j["quantization_note"] =
                "p50 只有 " + std::to_string(ticks) + " 个时钟 tick（粒度 " +
                std::to_string(granularity_ns) + " ns），量化相对误差约 ±" +
                std::to_string(rel_err * 100.0) +
                "%，分位数的尾数是量化产物。判断真实平均成本请用 amortized_ns。";
        }
    }
    return j;
}

/*
 * 计时器本身的开销基线。
 *
 * 单次 submit_order 只有几百纳秒，而 steady_clock::now() 自己就要几十纳秒。
 * 不测这个基线并扣掉，报出来的 p50 里有可观一部分是计时器的。
 */
double timer_overhead_ns(int n = 200000) {
    std::vector<double> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto a = Clock::now();
        auto b = Clock::now();
        v.push_back(std::chrono::duration<double, std::nano>(b - a).count());
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];   // 中位数
}

// ── 1. 撮合吞吐 + 单笔下单延迟 ──────────────────────────────
//
// 订单流构造：70% 挂在盘口附近的 LIMIT（大多会挂上去），
// 20% 穿价 LIMIT（会吃对手盘），10% MARKET。
// 这个配比是为了让簿既能长大、又持续有成交 —— 全是挂单的话测的是插入，
// 全是市价单的话簿会被吃空，两种都不是真实盘口。
json bench_submit(int n_orders, double overhead_ns, double granularity_ns) {
    /*
     * 预热。
     *
     * 原来三个用例都是从第 0 次迭代就开始计时，于是前若干次里混着一次性成本：
     * 指令缓存冷、分支预测器没训练、allocator 的 arena 还没长起来、
     * Session/deque 的首批堆分配。这些会抬高 p99/p999 —— 而这个 benchmark
     * 的全部意义就在尾部分位数上。
     *
     * 预热用**另一个 Session**，不污染被测那一份的簿状态。
     */
    {
        Session warm("warmup", "TEST");
        warm.seed_orders(5000, Price::from_double(100.0), TickSize::from_double(0.01), 2, 200, 100, 1000, 7u);
        Lcg wr(1234567);
        const int wn = std::max(2000, n_orders / 20);
        for (int i = 0; i < wn; ++i) {
            BookOrder o;
            o.order_id = "w" + std::to_string(i);
            o.side = (wr.next() % 2) ? Side::BUY : Side::SELL;
            o.quantity = wr.range(100, 1000);
            o.order_type = OrderType::LIMIT;
            double off = wr.range(1, 150) * 0.01;
            o.price = Price::from_double((o.side == Side::BUY) ? 100.0 - off : 100.0 + off);
            o.timestamp = static_cast<int64_t>(i);
            warm.submit_order(std::move(o));
        }
    }

    Session session("bench", "TEST");
    session.seed_orders(5000, Price::from_double(100.0), TickSize::from_double(0.01), 2, 200, 100, 1000, 20240824u);

    Lcg rng(20240824);
    std::vector<double> lat;
    lat.reserve(n_orders);

    int filled_total = 0;
    auto t_start = Clock::now();
    for (int i = 0; i < n_orders; ++i) {
        BookOrder o;
        o.order_id = "b" + std::to_string(i);
        o.side = (rng.next() % 2) ? Side::BUY : Side::SELL;
        o.quantity = rng.range(100, 1000);
        o.timestamp = static_cast<int64_t>(i);
        o.client_tag = "bench";

        uint64_t roll = rng.next() % 100;
        if (roll < 70) {                       // 被动挂单
            o.order_type = OrderType::LIMIT;
            double off = rng.range(1, 150) * 0.01;
            o.price = Price::from_double((o.side == Side::BUY) ? 100.0 - off : 100.0 + off);
        } else if (roll < 90) {                // 主动穿价
            o.order_type = OrderType::LIMIT;
            double off = rng.range(0, 30) * 0.01;
            o.price = Price::from_double((o.side == Side::BUY) ? 100.0 + off : 100.0 - off);
        } else {                               // 市价
            o.order_type = OrderType::MARKET;
            o.price = Price::from_double(0.0);
        }

        auto a = Clock::now();
        MatchResult r = session.submit_order(std::move(o));
        auto b = Clock::now();
        lat.push_back(std::chrono::duration<double, std::nano>(b - a).count() - overhead_ns);
        filled_total += r.filled_quantity;
    }
    auto t_end = Clock::now();

    double secs = std::chrono::duration<double>(t_end - t_start).count();
    json j;
    j["orders"] = n_orders;
    j["wall_seconds"] = secs;
    j["orders_per_sec"] = n_orders / secs;
    j["filled_quantity_total"] = filled_total;
    j["latency"] = pct_json(percentiles(lat), granularity_ns);
    j["amortized_ns"] = std::chrono::duration<double, std::nano>(t_end - t_start).count() / n_orders;
    j["book_orders_after"] = session.get_stats(10).bid_depth + session.get_stats(10).ask_depth;
    return j;
}

// ── 2. 盘口深度查询随簿深度的变化 ────────────────────────────
//
// get_depth 走的是价格树的前 N 档。簿里挂单从 1k 涨到 100k 时，
// 这个查询会不会跟着变慢？—— 如果实现是对的，它应该只跟 levels 有关，
// 跟簿总大小无关。这条曲线就是在验证这件事。
json bench_depth(int book_size, double overhead_ns, double granularity_ns,
                 int queries = 20000) {
    Session session("depth", "TEST");
    session.seed_orders(book_size, Price::from_double(100.0), TickSize::from_double(0.01), 2, 2000, 100, 1000, 31337u);

    // 预热：先跑一批不计时的查询，把 icache / 分支预测器带热
    for (int i = 0; i < 2000; ++i) {
        volatile auto d = session.get_depth(10);
        (void)d;
    }

    std::vector<double> lat;
    lat.reserve(queries);
    for (int i = 0; i < queries; ++i) {
        auto a = Clock::now();
        volatile auto d = session.get_depth(10);
        auto b = Clock::now();
        (void)d;
        lat.push_back(std::chrono::duration<double, std::nano>(b - a).count() - overhead_ns);
    }

    // 摊销计时：整批只取两次时间戳，量化误差被 queries 摊掉
    auto ta = Clock::now();
    for (int i = 0; i < queries; ++i) {
        volatile auto d = session.get_depth(10);
        (void)d;
    }
    auto tb = Clock::now();
    double amortized = std::chrono::duration<double, std::nano>(tb - ta).count() / queries;

    json j;
    j["amortized_ns"] = amortized;
    j["book_size_requested"] = book_size;
    BookStats st = session.get_stats(10);
    j["bid_depth"] = st.bid_depth;
    j["ask_depth"] = st.ask_depth;
    j["queries"] = queries;
    j["latency"] = pct_json(percentiles(lat), granularity_ns);
    return j;
}

// ── 3. 撤单延迟 ───────────────────────────────────────────
//
// 挂一批远离盘口的单（保证不会被吃掉），再逐个撤，测撤单本身的成本。
json bench_cancel(int n, double overhead_ns, double granularity_ns) {
    // 预热：在另一个 session 上完整跑一遍「挂单 → 撤单」，
    // 把索引哈希表的桶、deque 的堆块都先分配起来
    {
        Session warm("cancel_warm", "TEST");
        std::vector<std::string> wids;
        for (int i = 0; i < 2000; ++i) {
            BookOrder o;
            o.order_id = "cw" + std::to_string(i);
            o.side = Side::BUY;
            o.order_type = OrderType::LIMIT;
            o.price = Price::from_double(50.0 - (i % 1000) * 0.01);
            o.quantity = 100;
            o.timestamp = static_cast<int64_t>(i);
            wids.push_back(o.order_id);
            warm.submit_order(std::move(o));
        }
        for (const auto& id : wids) warm.cancel_order(id);
    }

    Session session("cancel", "TEST");
    session.seed_orders(5000, Price::from_double(100.0), TickSize::from_double(0.01), 2, 200, 100, 1000, 987654u);

    std::vector<std::string> ids;
    ids.reserve(n);
    for (int i = 0; i < n; ++i) {
        BookOrder o;
        o.order_id = "c" + std::to_string(i);
        o.side = Side::BUY;
        o.order_type = OrderType::LIMIT;
        o.price = Price::from_double(50.0 - (i % 1000) * 0.01);   // 远低于盘口，绝不会成交
        o.quantity = 100;
        o.timestamp = static_cast<int64_t>(i);
        o.client_tag = "bench";
        ids.push_back(o.order_id);
        session.submit_order(std::move(o));
    }

    std::vector<double> lat;
    lat.reserve(n);
    int ok = 0;
    // ⚠️ 撤单是一次性的（撤过的单不能再撤），没法像深度查询那样重跑一遍取摊销。
    //    所以这里在逐次计时的**外面**再套一层总计时，用总耗时÷n 得到摊销值。
    auto ta = Clock::now();
    for (const auto& id : ids) {
        auto a = Clock::now();
        bool r = session.cancel_order(id);
        auto b = Clock::now();
        lat.push_back(std::chrono::duration<double, std::nano>(b - a).count() - overhead_ns);
        ok += r ? 1 : 0;
    }
    auto tb = Clock::now();

    json j;
    j["amortized_ns"] = std::chrono::duration<double, std::nano>(tb - ta).count() / n;
    j["orders"] = n;
    j["cancelled_ok"] = ok;
    j["latency"] = pct_json(percentiles(lat), granularity_ns);
    return j;
}

}  // namespace

/*
 * 环境元数据。
 *
 * 原来 orderbook.json **完全没有 environment 块** —— 于是 make_report.py 渲染
 * 环境表时只能借 backtest.json 的那一份，两份数据可能来自不同机器、不同日期，
 * 而读报告的人完全无从分辨。一份自称可复现的报告不该有这种缺口。
 */
json environment(double overhead_ns, double granularity_ns) {
    json e;
#if defined(__clang_version__)
    e["compiler"] = std::string("clang ") + __clang_version__;
#elif defined(__VERSION__)
    e["compiler"] = std::string("gcc ") + __VERSION__;
#else
    e["compiler"] = "unknown";
#endif
#if defined(__APPLE__)
    e["os"] = "macOS";
#elif defined(__linux__)
    e["os"] = "Linux";
#else
    e["os"] = "unknown";
#endif
#if defined(NDEBUG)
    e["ndebug"] = true;
#else
    e["ndebug"] = false;
#endif
#if defined(BENCH_BUILD_TYPE)
    e["cmake_build_type"] = BENCH_BUILD_TYPE;
#endif
#if defined(__OPTIMIZE__)
    e["optimized"] = true;
#else
    e["optimized"] = false;
#endif
    e["clock"] = "std::chrono::steady_clock";
    e["clock_granularity_ns"] = granularity_ns;
    e["timer_overhead_ns"] = overhead_ns;
    e["fp_contract"] = "off (见 CMakeLists 的 -ffp-contract=off)";
    return e;
}

int main(int argc, char* argv[]) {
    const int scale = argc > 1 ? std::atoi(argv[1]) : 1;

    const double granularity = clock_granularity_ns();
    const double overhead = timer_overhead_ns();
    std::cerr << "时钟粒度（实测最小非零差值）: " << granularity << " ns\n";
    std::cerr << "计时器开销基线（中位数）    : " << overhead << " ns";
    if (overhead <= 0.0) {
        std::cerr << "  ⚠️ 中位开销落在时钟粒度以下，扣除实际为 0";
    }
    std::cerr << "\n";

    json out;
    out["timer_overhead_ns"] = overhead;
    out["clock_granularity_ns"] = granularity;
    // 显式记录「扣除是否真的生效」。原来这里只有 timer_overhead_ns，
    // 已提交的那次运行里它是 0.0，也就是什么都没扣 —— 但结果文件里看不出
    // 这是「开销真的为零」还是「开销小于时钟粒度、测不出来」。
    out["timer_overhead_subtraction_effective"] = (overhead > 0.0);
    out["environment"] = environment(overhead, granularity);

    out["submit"] = json::array();
    for (int n : {50'000 * scale, 200'000 * scale}) {
        std::cerr << "  撮合吞吐 " << n << " 单...\n";
        out["submit"].push_back(bench_submit(n, overhead, granularity));
    }

    out["depth_query"] = json::array();
    for (int b : {1'000, 10'000, 100'000}) {
        std::cerr << "  深度查询 簿内 " << b << " 单...\n";
        out["depth_query"].push_back(bench_depth(b, overhead, granularity));
    }

    std::cerr << "  撤单延迟...\n";
    out["cancel"] = bench_cancel(50'000 * scale, overhead, granularity);

    std::cout << out.dump(2) << std::endl;
    return 0;
}
