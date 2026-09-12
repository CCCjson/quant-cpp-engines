/*
 * test_price.cpp — 定点价格类型的单元测试
 *
 * Price 是这次改动里**唯一全新的逻辑**，所以它在有使用者之前先有测试。
 * 重点验证两条性质，它们正是引入这个类型的全部理由：
 *   1. 代数等价的算式给出**同一个** Price（浮点键缺陷的直接反面）
 *   2. double ↔ 定点的往返无损
 */

#include <gtest/gtest.h>

#include "orderbook/price.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>

using namespace orderbook;

namespace {
std::uint64_t bits(double d) {
    std::uint64_t u = 0;
    std::memcpy(&u, &d, sizeof u);
    return u;
}
}  // namespace

TEST(PriceTest, ConstructionAndConversion) {
    EXPECT_EQ(Price::from_double(100.07).raw(), 1000700);
    EXPECT_EQ(Price::from_raw(1000700).to_double(), 100.07);
    EXPECT_EQ(Price().raw(), 0);
    // 负价格也能表示（撮合层会拒绝，但类型本身不该悄悄截断）
    EXPECT_EQ(Price::from_double(-1.5).raw(), -15000);
}

/*
 * ── 核心性质 1：代数等价的算式必须给出同一个 Price ──
 *
 * 这正是浮点键缺陷的反面。用 double 时这两个式子给出不同的位模式：
 *     100.00 + 7*0.01                    = 100.06999999999999
 *     round((100.00 + 7*0.01)/0.01)*0.01 = 100.07000000000001
 * 转成 Price 之后必须collapse 成同一个值。
 */
TEST(PriceTest, AlgebraicallyEqualFormulasCollapseToTheSamePrice) {
    const double direct = 100.00 + 7 * 0.01;
    const double via_round = std::round((100.00 + 7 * 0.01) / 0.01) * 0.01;

    // 前提：这两个 double 确实不同（否则这条测试没在测东西）
    ASSERT_NE(bits(direct), bits(via_round))
        << "两个算式给出了相同的 double —— 复现前提失效，需要换一组价格";

    EXPECT_EQ(Price::from_double(direct), Price::from_double(via_round))
        << "定点化之后，同一个名义价格必须是同一个值";

    // 而且在有序容器里是同一个 key —— 这是订单簿真正依赖的性质
    std::map<Price, int> book;
    book[Price::from_double(direct)] += 97;
    book[Price::from_double(via_round)] += 417;
    EXPECT_EQ(book.size(), 1u) << "同一名义价格必须只占一个档位";
    EXPECT_EQ(book.begin()->second, 97 + 417);
}

/*
 * ── 核心性质 2：往返无损 ──
 *
 * raw/10000.0 的分子分母都可精确表示，IEEE 754 保证结果是最接近该十进制值的
 * double —— 与直接解析同一个字面量得到的位模式相同。这里逐个核对。
 */
TEST(PriceTest, DoubleRoundTripIsLossless) {
    const double samples[] = {99.99, 99.96, 100.07, 101.50, 100.02,
                              0.0001, 12345.6789, 0.0, 1.0, -3.25};
    for (double d : samples) {
        const Price p = Price::from_double(d);
        EXPECT_EQ(bits(p.to_double()), bits(d))
            << "往返改变了位模式: " << d << " -> " << p.to_double();
    }
}

// 网格上连续 tick 的往返也必须无损（订单簿会大量生成这类价格）
TEST(PriceTest, TickGridRoundTripsLosslessly) {
    const Price base = Price::from_double(100.00);
    const TickSize tick = TickSize::from_double(0.01);
    std::set<std::int64_t> seen;
    for (int i = -500; i <= 500; ++i) {
        const Price p = base + Price::from_raw(tick.raw) * i;
        // 每个 tick 都是不同的 raw 值 —— 不会有两个 tick 塌成一个
        EXPECT_TRUE(seen.insert(p.raw()).second) << "tick " << i << " 与另一个 tick 撞了";
        // 转成 double 再转回来必须原样
        EXPECT_EQ(Price::from_double(p.to_double()), p) << "tick " << i << " 往返丢失";
    }
    EXPECT_EQ(seen.size(), 1001u);
}

TEST(PriceTest, ComparisonIsExactIntegerComparison) {
    EXPECT_LT(Price::from_double(99.99), Price::from_double(100.00));
    EXPECT_GT(Price::from_double(100.01), Price::from_double(100.00));
    EXPECT_EQ(Price::from_double(100.00), Price::from_raw(1000000));
    EXPECT_LE(Price::from_double(1.0), Price::from_double(1.0));
    EXPECT_GE(Price::from_double(1.0), Price::from_double(1.0));
    EXPECT_NE(Price::from_double(1.0), Price::from_double(1.0001));
}

TEST(PriceTest, ArithmeticStaysOnTheGrid) {
    const Price a = Price::from_double(100.00);
    const Price b = Price::from_double(0.25);
    EXPECT_EQ((a + b).to_double(), 100.25);
    EXPECT_EQ((a - b).to_double(), 99.75);
    EXPECT_EQ((b * 4).to_double(), 1.00);
    EXPECT_EQ((4 * b).to_double(), 1.00);
}

TEST(PriceTest, IsMultipleOfTick) {
    const std::int64_t cent = TickSize::from_double(0.01).raw;
    EXPECT_TRUE(Price::from_double(100.07).is_multiple_of(cent));
    EXPECT_TRUE(Price::from_double(100.00).is_multiple_of(cent));
    EXPECT_FALSE(Price::from_double(100.005).is_multiple_of(cent));
    // tick 为 0 或负数视为无效，一律返回 false（不做除零）
    EXPECT_FALSE(Price::from_double(100.00).is_multiple_of(0));
    EXPECT_FALSE(Price::from_double(100.00).is_multiple_of(-1));
}

// map 的降序比较器（买盘用）必须能直接用 std::greater<Price>
TEST(PriceTest, WorksAsOrderedMapKeyInBothDirections) {
    std::map<Price, int, std::greater<Price>> bids;
    for (double d : {99.98, 100.00, 99.99}) bids[Price::from_double(d)] = 1;
    EXPECT_EQ(bids.begin()->first, Price::from_double(100.00)) << "买盘 begin() 应是最高价";

    std::map<Price, int> asks;
    for (double d : {101.00, 100.50, 101.50}) asks[Price::from_double(d)] = 1;
    EXPECT_EQ(asks.begin()->first, Price::from_double(100.50)) << "卖盘 begin() 应是最低价";
}
