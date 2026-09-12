/*
 * test_price_integrity.cpp — 价格表示的回归测试
 *
 * ============================================================
 * 这些用例是怎么来的
 * ============================================================
 *
 * 不是人读代码读出来的，是 test_differential.cpp 的随机化差分测试
 * **自己发现的**：种子 12648430，原始 300 步，自动收缩到 3 步。
 *
 * 参照模型按整数 tick 归档价格，当时的真实簿按 double 归档，两者一比就暴露了。
 *
 * 收缩后的最小复现（下面 SameNominalPriceIsOneLevel 就是它）：
 *     LIMIT BUY  97 股 @ 100.07   （价格算式一）
 *     LIMIT BUY  417 股 @ 100.07  （价格算式二）
 *     LIMIT SELL 153 股 @ 99.94
 *   当时真实簿产生 1 笔成交，参照模型产生 2 笔。
 *
 * ============================================================
 * 当时的根因
 * ============================================================
 *
 * 订单簿曾是 std::map<double, PriceLevel> —— 价格是浮点数，且被当作
 * **有序 map 的 key**。两个代数上相等的算式给出不同的 double：
 *
 *     100.00 + 7*0.01                      = 100.06999999999999
 *     round((100.00 + 7*0.01)/0.01)*0.01   = 100.07000000000001
 *
 * 后者正是当时 Session::seed_orders 用的写法。于是「100.07 这一档」成了
 * 两个不同的 key。两个后果，第二个严重得多：
 *
 *   1. 档位数不对，bid_quantity_at(100.07) 只报其中一半的量。
 *   2. **时间优先被破坏** —— 同一名义价格上的两个订单落在不同档位，
 *      撮合按 double 大小取「最优」档，于是后到的订单先成交、先到的被跳过。
 *      实测 FIRST(97股, ts=1) 被跳过，SECOND(417股, ts=2) 吃掉全部 153 股。
 *      而 README 明确承诺「价格优先、时间优先（同价位 FIFO）」——
 *      这不是精度瑕疵，是违反了自己声明的核心撮合语义。
 *
 * ============================================================
 * 现状：已修复，这几个用例是那次修复的验收标准
 * ============================================================
 *
 * 价格已改成 int64 定点的强类型（见 orderbook/price.h），map 的 key 变成整数，
 * double↔定点的转换只发生在 JSON 边界，seed_orders 里那道
 * `round(p/tick)*tick` 的量化**整个消失了** —— 没有东西需要再被四舍五入。
 *
 * 这几个用例原来带 DISABLED_ 前缀（断言的是「修好之后应有的行为」，
 * 所以当时必然失败；长期红着的 CI 等于没有 CI，所以用 GoogleTest 的
 * DISABLED_ 惯例挂着而不是让它红）。修复落地后前缀已去掉，它们现在是
 * 常规回归测试：如果哪天有人把价格改回浮点，它们会立刻变红。
 */

#include <gtest/gtest.h>

#include "orderbook/limit_order_book.h"
#include "orderbook/matching_engine.h"
#include "test_price_helpers.h"

#include <cmath>
#include <string>

using namespace orderbook;

namespace {

// 同一名义价格 100.07 的两种算法。代数等价，浮点不等。
double price_direct() { return 100.00 + 7 * 0.01; }
double price_via_round() { return std::round((100.00 + 7 * 0.01) / 0.01) * 0.01; }

BookOrder limit_order(const std::string& id, Side side, double price, int qty,
                      std::int64_t ts) {
    BookOrder o;
    o.order_id = id;
    o.side = side;
    o.order_type = OrderType::LIMIT;
    o.price = P(price);
    o.quantity = qty;
    o.timestamp = ts;
    return o;
}

}  // namespace

/*
 * 前提校验：这个用例是**启用**的，且必须通过。
 *
 * 它断言的不是「簿的行为正确」，而是「这两个算式确实给出不同的 double」——
 * 也就是下面两个  用例的前提。
 *
 * 如果哪天编译器/优化选项变了、两个算式碰巧给出相同的位模式，那么下面两个
 * 用例就变成了空测试（永远通过，但什么也没验证）。这个用例会在那时变红，
 * 提醒「复现前提已失效，需要换一组价格重新构造」。
 *
 * 这一层元校验是必要的：一个悄悄失去意义的测试比没有测试更危险。
 */
TEST(PriceIntegrityTest, TwoAlgebraicallyEqualPriceFormulasDifferInBits) {
    const double a = price_direct();
    const double b = price_via_round();
    EXPECT_NE(a, b)
        << "两个算式给出了相同的 double（" << a << "），\n"
        << "下面两个  用例的复现前提已失效，需要换一组价格重新构造。";
    // 但它们的名义价格确实都是 100.07（四舍五入到分）
    EXPECT_EQ(std::llround(a * 100.0), 10007);
    EXPECT_EQ(std::llround(b * 100.0), 10007);
}

/*
 * 后果一：同一名义价格被拆成两档。
 * 差分测试收缩出的最小复现，原样固化。
 */
TEST(PriceIntegrityTest, SameNominalPriceIsOneLevel) {
    LimitOrderBook book;
    MatchingEngine engine;

    engine.submit_order(book, limit_order("FIRST", Side::BUY, price_direct(), 97, 1));
    engine.submit_order(book, limit_order("SECOND", Side::BUY, price_via_round(), 417, 2));

    const DepthSnapshot d = book.get_depth(10);
    EXPECT_EQ(d.bids.size(), 1u)
        << "两个订单都挂在名义价格 100.07，应当只有一档买盘。\n"
           "实测被拆成了 " << d.bids.size() << " 档 —— std::map<double, ...> "
           "把两个本应相等的价格当成了不同的 key。";

    // 该档的总量必须是两单之和；现在会各报一半
    EXPECT_EQ(book.bid_quantity_at(P(price_direct())), 97 + 417)
        << "bid_quantity_at(P(100.07)) 漏报了另一档的量。";
}

/*
 * 后果二（更严重）：时间优先被破坏。
 *
 * FIRST 先到（timestamp=1），SECOND 后到（timestamp=2），同价。
 * 卖 153 股时，按「同价位 FIFO」必须先吃完 FIRST 的 97 股，
 * 再从 SECOND 吃 56 股 —— 即 2 笔成交。
 */
TEST(PriceIntegrityTest, TimePriorityHoldsForSameNominalPrice) {
    LimitOrderBook book;
    MatchingEngine engine;

    engine.submit_order(book, limit_order("FIRST", Side::BUY, price_direct(), 97, 1));
    engine.submit_order(book, limit_order("SECOND", Side::BUY, price_via_round(), 417, 2));

    const MatchResult r =
        engine.submit_order(book, limit_order("SWEEP", Side::SELL, 99.94, 153, 3));

    ASSERT_EQ(r.fills.size(), 2u)
        << "同价位 FIFO 要求先吃满 FIRST(97) 再吃 SECOND(56)，共 2 笔。\n"
           "实测 " << r.fills.size() << " 笔 —— 两单落在不同 double 档位，"
           "撮合按档位价格取优，先到的那一单被跳过了。";

    EXPECT_EQ(r.fills[0].buy_order_id, "FIRST")
        << "第一笔必须成交给先到的 FIRST（时间优先）。";
    EXPECT_EQ(r.fills[0].quantity, 97);
    EXPECT_EQ(r.fills[1].buy_order_id, "SECOND");
    EXPECT_EQ(r.fills[1].quantity, 56);
}
