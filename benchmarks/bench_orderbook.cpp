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

json pct_json(Percentiles p) {
    return {{"p50_ns", p.p50}, {"p90_ns", p.p90}, {"p99_ns", p.p99},
            {"p999_ns", p.p999}, {"max_ns", p.max}, {"mean_ns", p.mean}};
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
json bench_submit(int n_orders, double overhead_ns) {
    Session session("bench", "TEST");
    session.seed_orders(5000, 100.0, 0.01, 2, 200);

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
            o.price = (o.side == Side::BUY) ? 100.0 - off : 100.0 + off;
        } else if (roll < 90) {                // 主动穿价
            o.order_type = OrderType::LIMIT;
            double off = rng.range(0, 30) * 0.01;
            o.price = (o.side == Side::BUY) ? 100.0 + off : 100.0 - off;
        } else {                               // 市价
            o.order_type = OrderType::MARKET;
            o.price = 0.0;
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
    j["latency"] = pct_json(percentiles(lat));
    j["book_orders_after"] = session.get_stats(10).bid_depth + session.get_stats(10).ask_depth;
    return j;
}

// ── 2. 盘口深度查询随簿深度的变化 ────────────────────────────
//
// get_depth 走的是价格树的前 N 档。簿里挂单从 1k 涨到 100k 时，
// 这个查询会不会跟着变慢？—— 如果实现是对的，它应该只跟 levels 有关，
// 跟簿总大小无关。这条曲线就是在验证这件事。
json bench_depth(int book_size, double overhead_ns, int queries = 20000) {
    Session session("depth", "TEST");
    session.seed_orders(book_size, 100.0, 0.01, 2, 2000);

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
    j["latency"] = pct_json(percentiles(lat));
    return j;
}

// ── 3. 撤单延迟 ───────────────────────────────────────────
//
// 挂一批远离盘口的单（保证不会被吃掉），再逐个撤，测撤单本身的成本。
json bench_cancel(int n, double overhead_ns) {
    Session session("cancel", "TEST");
    session.seed_orders(5000, 100.0, 0.01, 2, 200);

    std::vector<std::string> ids;
    ids.reserve(n);
    for (int i = 0; i < n; ++i) {
        BookOrder o;
        o.order_id = "c" + std::to_string(i);
        o.side = Side::BUY;
        o.order_type = OrderType::LIMIT;
        o.price = 50.0 - (i % 1000) * 0.01;   // 远低于盘口，绝不会成交
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
    j["latency"] = pct_json(percentiles(lat));
    return j;
}

}  // namespace

int main(int argc, char* argv[]) {
    const int scale = argc > 1 ? std::atoi(argv[1]) : 1;

    double overhead = timer_overhead_ns();
    std::cerr << "计时器开销基线（中位数）: " << overhead << " ns —— 已从所有延迟中扣除\n";

    json out;
    out["timer_overhead_ns"] = overhead;

    out["submit"] = json::array();
    for (int n : {50'000 * scale, 200'000 * scale}) {
        std::cerr << "  撮合吞吐 " << n << " 单...\n";
        out["submit"].push_back(bench_submit(n, overhead));
    }

    out["depth_query"] = json::array();
    for (int b : {1'000, 10'000, 100'000}) {
        std::cerr << "  深度查询 簿内 " << b << " 单...\n";
        out["depth_query"].push_back(bench_depth(b, overhead));
    }

    std::cerr << "  撤单延迟...\n";
    out["cancel"] = bench_cancel(50'000 * scale, overhead);

    std::cout << out.dump(2) << std::endl;
    return 0;
}
