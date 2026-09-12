/*
 * test_price_helpers.h — 测试里构造 Price 的简写
 *
 * 迁移到定点价格之后，测试里所有价格字面量都要包一层。用一个短函数 P()
 * 而不是自定义字面量 `101.00_p`：后者会在字面量处重新引入浮点转换，
 * 而这里恰恰是要消灭浮点路径的地方。
 *
 * 顺带一个收益：EXPECT_EQ(fill.price, P(101.00)) 现在是**精确的整数比较**，
 * 而原来 EXPECT_EQ(fill.price, 101.00) 是浮点相等比较 —— 那本来就是个隐患。
 */
#pragma once

#include "orderbook/price.h"

namespace orderbook {

inline Price P(double d) { return Price::from_double(d); }
inline TickSize TS(double d) { return TickSize::from_double(d); }

}  // namespace orderbook
