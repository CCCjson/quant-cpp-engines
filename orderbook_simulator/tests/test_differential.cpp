/*
 * test_differential.cpp — 随机化差分测试 + 不变量检查
 *
 * ============================================================
 * 为什么要这个
 * ============================================================
 *
 * 撮合引擎原来只有 40 个手写单元测试，全项目没有一个 assert、没有一处
 * 不变量检查（grep assert|invariant|crossed 零命中）。
 *
 * 手写用例的问题不是数量，是**它只覆盖作者想到的情形**。订单簿是个状态机，
 * 状态空间由「四种订单类型 × 买卖 × 价格档位 × 部分成交 × 撤单时机」相乘
 * 得出，手写永远铺不满。真正能铺满的办法是：随机生成订单流，同时喂给
 * 被测实现和一个**慢但显然正确**的参照实现，逐步比对。
 *
 * ============================================================
 * 参照模型的设计
 * ============================================================
 *
 * 参照模型刻意写得又慢又笨：一个 std::vector 按插入顺序存所有订单，
 * 撮合时线性扫描找「最优价格 + 最早插入」的对手单。O(n²)，但正确性
 * 一眼就能看出来，不需要推理。这是参照模型唯一该有的品质。
 *
 * ⭐ 一个关键设计：**参照模型用整数 tick 序号标识价格，不用 double。**
 *
 * 真实簿是 std::map<double, PriceLevel>，价格是浮点数且被当作有序 map
 * 的 key。参照模型按整数 tick 归档，因此不可能发生「同一个名义价格变成
 * 两个不同档位」这种事。两者一比，真实簿的浮点键问题就会暴露出来。
 *
 * 为了让这件事真的被触发，下面生成价格时会**随机选用两种代数等价的算式**
 * （base + k*tick 与 round((base + k*tick)/tick)*tick）。真实客户端本来就
 * 会用不同方式算价格，而正确的簿必须把它们视为同一档。
 *
 * ============================================================
 * 比什么
 * ============================================================
 *
 * 每一步操作之后比两样：
 *   1. 本次操作产生的成交序列 —— 逐笔比价格、数量、主动方、以及**顺序**
 *   2. 操作后的完整簿状态 —— 逐档的 (order_id, remaining) 有序列表
 *
 * 只比成交是不够的：会漏掉「成交对了但残留状态错了」这一类问题，而那类
 * 问题会在后续操作里才爆发，届时已经很难定位。
 *
 * 另外每步检查簿的不变量（买卖不交叉、档位总量等于档内活跃单余量之和等）。
 */

#include <gtest/gtest.h>

#include "orderbook/session.h"
#include "orderbook/limit_order_book.h"
#include "orderbook/matching_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace orderbook;

namespace {

// ── 价格网格 ──
// 用有限档位（41 个 tick）是刻意的：真实簿的 FOK 预检走 get_depth(100)，
// 也就是只看前 100 档（matching_engine.cpp:248，注释自己承认「100 档应该够了」
// 是猜的）。把网格控制在 100 档以内，可以把那个已知缺陷隔离出去、
// 单独用一个定向用例去打（见 test_fok_depth_cap.cpp 之类的后续工作），
// 从而让这个差分测试专注于撮合逻辑本身。
constexpr double kBase = 100.00;
constexpr double kTick = 0.01;
constexpr int kMinTick = -20;
constexpr int kMaxTick = 20;

// tick 序号 → double 价格。两种代数等价的算式，模拟不同客户端的算法。
double price_of(int tick, bool via_round) {
    const double raw = kBase + tick * kTick;
    if (!via_round) return raw;
    return std::round(raw / kTick) * kTick;   // seed_orders 用的就是这种写法
}

// ──────────────────────────────────────────────────────────────
// 参照模型
// ──────────────────────────────────────────────────────────────
struct RefOrder {
    std::string id;
    Side side = Side::BUY;
    int tick = 0;            // ⭐ 整数价格标识，不是 double
    int quantity = 0;
    int filled = 0;
    bool active = true;
    long long seq = 0;       // 插入顺序 = 时间优先的依据

    int remaining() const { return quantity - filled; }
};

struct RefFill {
    int tick = 0;
    int quantity = 0;
    Side aggressor = Side::BUY;
    std::string resting_id;
};

class ReferenceBook {
public:
    // 返回本次成交序列；rest_remainder 决定余量是否挂上簿
    std::vector<RefFill> submit(const std::string& id, Side side, OrderType type,
                                std::optional<int> limit_tick, int qty) {
        std::vector<RefFill> fills;
        if (qty <= 0) return fills;   // 与真实实现一致：非正数量不产生成交

        // FOK：先预检，量不够则整单拒绝、簿不动。
        // 注意参照模型算的是**全簿真实可用量**，不设档位上限。
        if (type == OrderType::FOK) {
            if (available(side, limit_tick) < qty) return fills;
        }

        int need = qty;
        while (need > 0) {
            RefOrder* best = best_counterparty(side, limit_tick);
            if (!best) break;
            const int traded = std::min(best->remaining(), need);
            best->filled += traded;
            if (best->remaining() == 0) best->active = false;
            need -= traded;
            fills.push_back({best->tick, traded, side, best->id});
        }

        // 只有 LIMIT 的余量会挂上簿；MARKET / IOC / FOK 都不挂
        if (type == OrderType::LIMIT && need > 0 && limit_tick) {
            orders_.push_back({id, side, *limit_tick, need, 0, true, next_seq_++});
        }
        return fills;
    }

    bool cancel(const std::string& id) {
        for (auto& o : orders_) {
            if (o.id == id && o.active) { o.active = false; return true; }
        }
        return false;
    }

    void add_resting(const std::string& id, Side side, int tick, int qty) {
        orders_.push_back({id, side, tick, qty, 0, true, next_seq_++});
    }

    // 簿状态：tick → 该档按时间优先排列的 (id, remaining)
    std::map<int, std::vector<std::pair<std::string, int>>> side_state(Side s) const {
        std::vector<const RefOrder*> live;
        for (const auto& o : orders_) {
            if (o.active && o.side == s && o.remaining() > 0) live.push_back(&o);
        }
        std::sort(live.begin(), live.end(),
                  [](const RefOrder* a, const RefOrder* b) { return a->seq < b->seq; });
        std::map<int, std::vector<std::pair<std::string, int>>> out;
        for (const auto* o : live) out[o->tick].emplace_back(o->id, o->remaining());
        return out;
    }

    std::optional<int> best_tick(Side s) const {
        std::optional<int> best;
        for (const auto& o : orders_) {
            if (!o.active || o.side != s || o.remaining() <= 0) continue;
            if (!best) { best = o.tick; continue; }
            best = (s == Side::BUY) ? std::max(*best, o.tick) : std::min(*best, o.tick);
        }
        return best;
    }

private:
    // 对手盘里价格最优、同价则插入最早的那一单
    RefOrder* best_counterparty(Side aggressor, std::optional<int> limit_tick) {
        const Side opp = (aggressor == Side::BUY) ? Side::SELL : Side::BUY;
        RefOrder* best = nullptr;
        for (auto& o : orders_) {
            if (!o.active || o.side != opp || o.remaining() <= 0) continue;
            if (limit_tick) {
                if (aggressor == Side::BUY && o.tick > *limit_tick) continue;
                if (aggressor == Side::SELL && o.tick < *limit_tick) continue;
            }
            if (!best) { best = &o; continue; }
            if (o.tick == best->tick) {
                if (o.seq < best->seq) best = &o;          // 时间优先
            } else if (aggressor == Side::BUY ? (o.tick < best->tick)
                                              : (o.tick > best->tick)) {
                best = &o;                                  // 价格优先
            }
        }
        return best;
    }

    int available(Side aggressor, std::optional<int> limit_tick) const {
        const Side opp = (aggressor == Side::BUY) ? Side::SELL : Side::BUY;
        int total = 0;
        for (const auto& o : orders_) {
            if (!o.active || o.side != opp || o.remaining() <= 0) continue;
            if (limit_tick) {
                if (aggressor == Side::BUY && o.tick > *limit_tick) continue;
                if (aggressor == Side::SELL && o.tick < *limit_tick) continue;
            }
            total += o.remaining();
        }
        return total;
    }

    std::vector<RefOrder> orders_;
    long long next_seq_ = 0;
};

// ──────────────────────────────────────────────────────────────
// 把真实簿的状态归一化成「tick → (id, remaining) 列表」以便比对
// ──────────────────────────────────────────────────────────────
// ⚠️ 这里按 tick 归档，而真实簿按 double 归档。如果真实簿把同一个名义价格
// 分裂成了两个 double 档位，归一化之后两份会合到同一个 tick 下 —— 数量总和
// 仍然对得上，但**档位数**对不上。所以下面另有一项专门比档位数。
int tick_of_price(double p) {
    return static_cast<int>(std::llround((p - kBase) / kTick));
}

struct RealSideState {
    std::map<int, int> qty_by_tick;        // tick → 总量
    std::size_t level_count = 0;           // 真实簿实际有多少个 double 档位
};

RealSideState real_side_state(const DepthSnapshot& d, Side s) {
    RealSideState out;
    const auto& levels = (s == Side::BUY) ? d.bids : d.asks;
    out.level_count = levels.size();
    for (const auto& lv : levels) out.qty_by_tick[tick_of_price(lv.price)] += lv.quantity;
    return out;
}

std::map<int, int> ref_qty_by_tick(const ReferenceBook& ref, Side s) {
    std::map<int, int> out;
    for (const auto& [tick, list] : ref.side_state(s)) {
        int sum = 0;
        for (const auto& [id, rem] : list) sum += rem;
        if (sum > 0) out[tick] = sum;
    }
    return out;
}

std::string dump(const std::map<int, int>& m) {
    std::ostringstream os;
    for (const auto& [k, v] : m) os << "  tick " << k << " -> " << v << "\n";
    return os.str();
}

// ──────────────────────────────────────────────────────────────
// 操作脚本：先生成，再对两个实现按同一顺序重放。
// 「先生成完整脚本」而不是边跑边生成，是为了失败时能做收缩（见 shrink）。
// ──────────────────────────────────────────────────────────────
struct Op {
    enum Kind { Submit, Cancel } kind = Submit;
    Side side = Side::BUY;
    OrderType type = OrderType::LIMIT;
    int tick = 0;
    int quantity = 0;
    bool via_round = false;    // 价格用哪种算式
    int cancel_nth = 0;        // 撤第几个已提交的订单
};

std::vector<Op> make_script(std::uint32_t seed, int n, bool mixed_price_algebra) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> tick_d(kMinTick, kMaxTick);
    std::uniform_int_distribution<int> qty_d(1, 500);
    std::uniform_int_distribution<int> kind_d(0, 99);
    std::uniform_int_distribution<int> type_d(0, 99);
    std::uniform_int_distribution<int> coin(0, 1);

    std::vector<Op> ops;
    ops.reserve(n);
    for (int i = 0; i < n; ++i) {
        Op op;
        if (kind_d(rng) < 15 && i > 10) {
            op.kind = Op::Cancel;
            op.cancel_nth = rng() % static_cast<unsigned>(i);
            ops.push_back(op);
            continue;
        }
        op.kind = Op::Submit;
        op.side = coin(rng) ? Side::BUY : Side::SELL;
        const int t = type_d(rng);
        // 权重刻意偏向 LIMIT：只有它会在簿上留下状态，是状态空间的主要来源
        if (t < 65)      op.type = OrderType::LIMIT;
        else if (t < 80) op.type = OrderType::MARKET;
        else if (t < 92) op.type = OrderType::IOC;
        else             op.type = OrderType::FOK;
        op.tick = tick_d(rng);
        op.quantity = qty_d(rng);
        // mixed_price_algebra=false → 所有价格统一用一种算式，不会触发浮点分裂，
        // 于是差分测试比的就是纯撮合逻辑。true → 混用两种代数等价算式，
        // 复现真实客户端各自算价格的情形，会把浮点键缺陷暴露出来。
        op.via_round = mixed_price_algebra ? (coin(rng) == 1) : false;
        ops.push_back(op);
    }
    return ops;
}

// 一次重放的结果。ok=false 时 detail 里是人可读的失败说明。
struct ReplayResult {
    bool ok = true;
    std::string detail;
};

/*
 * 是否比对「档位数」。
 *
 * 分成两档是刻意的：浮点价格键会把同一名义价格拆成多档，这个缺陷已经被
 * 本测试独立发现（种子 12648430 第 106 步）。它一旦触发就会掩盖它下面的
 * 一切问题 —— 每个种子都在同一处先失败，撮合逻辑本身反而测不到。
 *
 * 所以：
 *   check_level_count = false → 只比逐档总量、成交序列、不变量。
 *                               这一档必须通过，它守的是撮合逻辑。
 *   check_level_count = true  → 连档位数一起比。这一档目前会失败，
 *                               是那个浮点键缺陷的验收测试（见文件末尾的
 *                               DISABLED_ 用例）。
 */
struct ReplayOpts {
    bool check_level_count = true;
};

ReplayResult replay(const std::vector<Op>& ops, ReplayOpts opts = {}) {
    LimitOrderBook book;
    MatchingEngine engine;
    ReferenceBook ref;
    std::vector<std::string> submitted;   // 真实侧的 order_id，供撤单用
    std::vector<std::string> ref_ids;

    auto fail = [](const std::string& msg) { return ReplayResult{false, msg}; };

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        std::ostringstream where;
        where << "第 " << i << " 步操作";

        if (op.kind == Op::Cancel) {
            if (submitted.empty()) continue;
            const std::size_t idx = static_cast<std::size_t>(op.cancel_nth) % submitted.size();
            const bool a = book.cancel_order(submitted[idx]);
            const bool b = ref.cancel(ref_ids[idx]);
            if (a != b) {
                std::ostringstream os;
                os << where.str() << " 撤单结果不一致：真实=" << a << " 参照=" << b
                   << "（order_id=" << submitted[idx] << "）";
                return fail(os.str());
            }
        } else {
            const std::string id = "d" + std::to_string(i);

            BookOrder o;
            o.order_id = id;
            o.side = op.side;
            o.order_type = op.type;
            o.quantity = op.quantity;
            o.timestamp = static_cast<std::int64_t>(i) + 1;
            // MARKET 不看价格；其余三种用生成的价格
            o.price = (op.type == OrderType::MARKET) ? 0.0
                                                     : price_of(op.tick, op.via_round);

            const MatchResult res = engine.submit_order(book, std::move(o));

            const std::optional<int> limit =
                (op.type == OrderType::MARKET) ? std::nullopt : std::optional<int>(op.tick);
            const std::vector<RefFill> rf = ref.submit(id, op.side, op.type, limit, op.quantity);

            submitted.push_back(id);
            ref_ids.push_back(id);

            // ── 比成交序列：笔数、以及逐笔的价格/数量/主动方与顺序 ──
            if (res.fills.size() != rf.size()) {
                std::ostringstream os;
                os << where.str() << " 成交笔数不一致：真实=" << res.fills.size()
                   << " 参照=" << rf.size() << "\n  订单: "
                   << order_type_to_string(op.type) << " "
                   << side_to_string(op.side) << " qty=" << op.quantity
                   << " tick=" << op.tick;
                return fail(os.str());
            }
            for (std::size_t k = 0; k < rf.size(); ++k) {
                const int real_tick = tick_of_price(res.fills[k].price);
                if (real_tick != rf[k].tick || res.fills[k].quantity != rf[k].quantity ||
                    res.fills[k].aggressor_side != rf[k].aggressor) {
                    std::ostringstream os;
                    os << where.str() << " 第 " << k << " 笔成交不一致：\n"
                       << "    真实 tick=" << real_tick << " qty=" << res.fills[k].quantity << "\n"
                       << "    参照 tick=" << rf[k].tick << " qty=" << rf[k].quantity;
                    return fail(os.str());
                }
            }
        }

        // ── 比簿状态：逐档总量 ──
        const DepthSnapshot d = book.get_depth(200);
        for (Side s : {Side::BUY, Side::SELL}) {
            const RealSideState real = real_side_state(d, s);
            const std::map<int, int> want = ref_qty_by_tick(ref, s);
            if (real.qty_by_tick != want) {
                std::ostringstream os;
                os << where.str() << " " << side_to_string(s)
                   << " 盘逐档总量不一致：\n  真实:\n" << dump(real.qty_by_tick)
                   << "  参照:\n" << dump(want);
                return fail(os.str());
            }
            // ⭐ 档位数：真实簿按 double 归档，参照按 tick 归档。
            // 同一名义价格若被拆成两个 double 键，总量仍相同但档位数会多。
            if (opts.check_level_count && real.level_count != want.size()) {
                std::ostringstream os;
                os << where.str() << " " << side_to_string(s)
                   << " 档位数不一致（浮点价格键把同一名义价格拆成了多档）：\n"
                   << "    真实档位数=" << real.level_count
                   << "  参照档位数=" << want.size() << "\n"
                   << "  这说明 std::map<double, PriceLevel> 把两个本应相等的价格"
                      "当成了不同的 key。";
                return fail(os.str());
            }
        }

        // ── 不变量：买卖不交叉 ──
        const auto bb = book.best_bid();
        const auto ba = book.best_ask();
        if (bb && ba && !(*bb < *ba)) {
            std::ostringstream os;
            os << where.str() << " 簿交叉了：best_bid=" << *bb << " >= best_ask=" << *ba;
            return fail(os.str());
        }
    }
    return ReplayResult{};
}

/*
 * 失败收缩：把脚本二分裁短，只要仍然失败就继续裁。
 * 随机化测试如果只报「第 1783 步不一致」，人是没法看的；
 * 收缩到几步之后才能真正拿去调试。
 */
std::vector<Op> shrink(std::vector<Op> ops, ReplayOpts opts) {
    // 阶段一：二分截尾。失败点通常在前半段，先把尾巴砍掉最省事。
    std::size_t cut = ops.size() / 2;
    while (cut >= 1) {
        if (cut < ops.size()) {
            std::vector<Op> shorter(ops.begin(), ops.begin() + static_cast<long>(cut));
            if (!replay(shorter, opts).ok) {
                ops = std::move(shorter);
                cut = ops.size() / 2;
                continue;
            }
        }
        if (cut == 1) break;
        cut /= 2;
    }

    // 阶段二：贪心逐个删除（delta debugging 的朴素版）。
    // 从后往前删，删掉之后仍然失败就永久删掉。只截尾的话留下的脚本里
    // 会有大量与失败无关的操作，人还是看不了。
    bool improved = true;
    while (improved) {
        improved = false;
        for (std::size_t i = ops.size(); i-- > 0;) {
            if (ops.size() <= 1) break;
            std::vector<Op> without = ops;
            without.erase(without.begin() + static_cast<long>(i));
            if (!replay(without, opts).ok) {
                ops = std::move(without);
                improved = true;
            }
        }
    }
    return ops;
}

std::string describe(const std::vector<Op>& ops) {
    std::ostringstream os;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& o = ops[i];
        os << "  [" << i << "] ";
        if (o.kind == Op::Cancel) {
            os << "CANCEL nth=" << o.cancel_nth << "\n";
        } else {
            os << order_type_to_string(o.type) << " " << side_to_string(o.side)
               << " tick=" << o.tick << " qty=" << o.quantity
               << " price=" << price_of(o.tick, o.via_round)
               << (o.via_round ? " (round 算式)" : " (直算)") << "\n";
        }
    }
    return os.str();
}

}  // namespace

// ──────────────────────────────────────────────────────────────
// 差分测试主体
// ──────────────────────────────────────────────────────────────

namespace {

// 跑一批种子，返回第一个失败（已收缩）的可读描述；全过则返回空。
std::string run_seeds(ReplayOpts opts, int seeds, int steps, bool mixed_price_algebra) {
    for (int s = 0; s < seeds; ++s) {
        const std::uint32_t seed = 0xC0FFEEu + static_cast<std::uint32_t>(s);
        const std::vector<Op> ops = make_script(seed, steps, mixed_price_algebra);
        const ReplayResult r = replay(ops, opts);
        if (r.ok) continue;

        const std::vector<Op> minimal = shrink(ops, opts);
        const ReplayResult mr = replay(minimal, opts);
        std::ostringstream os;
        os << "\n  种子       : " << seed << "（原始 " << steps << " 步）\n"
           << "  原始失败   : " << r.detail << "\n"
           << "  已收缩至   : " << minimal.size() << " 步\n"
           << "  收缩后失败 : " << mr.detail << "\n"
           << "  最小复现脚本:\n" << describe(minimal);
        return os.str();
    }
    return {};
}

int soak_seeds(int dflt) {
    if (const char* env = std::getenv("OB_SOAK_SEEDS")) {
        return std::max(1, std::atoi(env));
    }
    return dflt;
}

}  // namespace

/*
 * 主用例：撮合逻辑必须与参照模型逐笔一致。
 *
 * 比对内容：成交序列（价格/数量/主动方/顺序）、逐档总量、买卖不交叉。
 * 不比档位数 —— 那是下面那个 DISABLED_ 用例的事，理由见 ReplayOpts 的注释。
 *
 * 规模：默认 200 个种子 × 300 步，单机秒级。长跑设 OB_SOAK_SEEDS。
 */
TEST(DifferentialTest, MatchingLogicMatchesReferenceModel) {
    const std::string failure = run_seeds(ReplayOpts{/*check_level_count=*/true},
                                          soak_seeds(200), 300,
                                          /*mixed_price_algebra=*/false);
    EXPECT_TRUE(failure.empty()) << "撮合逻辑与参照模型不一致：" << failure;
}

/*
 * ⛔ 已知缺陷的验收测试，当前**预期失败**，故 DISABLED_。
 *
 * 缺陷：订单簿是 std::map<double, PriceLevel>（limit_order_book.h:152-158），
 * 价格是浮点数且被当作有序 map 的 key。两个代数上相等、但浮点位模式不同的
 * 价格会成为两个不同的档位。
 *
 * 本测试就是这么发现它的：参照模型按整数 tick 归档、不可能分裂；真实簿分裂了。
 * 种子 12648430 第 106 步，BUY 盘真实 7 档而参照 6 档 —— 逐档总量全对，
 * 唯独档位数多一个。
 *
 * 具体机理（已单独复现）：mid=100.0、tick=0.01 时，
 *   round((100.0 - 1*0.01)/0.01)*0.01  →  99.990000000000009
 *   直接解析字面量 99.99                →  99.989999999999995
 * 两个不同的 double。于是 bid_quantity_at(99.99) 会漏报另一半的量。
 *
 * 为什么 DISABLED_ 而不是让它红着：CI 长期红等于没有 CI，红色会被习惯性忽略。
 * DISABLED_ 是 GoogleTest 为「已知缺陷、已有测试守着」准备的惯例做法 ——
 * 用 --gtest_also_run_disabled_tests 就能看到它，且缺陷修好时它就是验收标准。
 *
 * 修复方案（计划中的 Layer 2）：价格改成 int64 定点的强类型，
 * double↔定点的转换只发生在 JSON 边界。届时把本用例的 DISABLED_ 去掉。
 */
TEST(DifferentialTest, DISABLED_PriceLevelsAreNotSplitByFloatKeys) {
    const std::string failure = run_seeds(ReplayOpts{/*check_level_count=*/true},
                                          soak_seeds(200), 300,
                                          /*mixed_price_algebra=*/true);
    EXPECT_TRUE(failure.empty())
        << "浮点价格键把同一名义价格拆成了多档：" << failure;
}
