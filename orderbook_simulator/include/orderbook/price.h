/*
 * price.h — 定点价格类型
 *
 * ============================================================
 * 为什么要有这个类型
 * ============================================================
 *
 * 订单簿原来是 `std::map<double, PriceLevel>` —— 价格是浮点数，而且被当作
 * **有序 map 的 key**。这带来一个真实的缺陷：两个代数上相等的算式给出不同的 double，
 *
 *     100.00 + 7*0.01                      = 100.06999999999999
 *     round((100.00 + 7*0.01)/0.01)*0.01   = 100.07000000000001
 *
 * 于是「100.07 这一档」在簿里成了两个不同的 key。后果不只是档位数不对，
 * 更严重的是**时间优先被破坏**：同价的两单落在不同档位，撮合按 double 大小取
 * 「最优」档，后到的订单可能先成交。而 README 明确承诺「价格优先、时间优先」。
 *
 * 这个缺陷是随机化差分测试自己发现的（见 tests/test_differential.cpp）。
 *
 * ============================================================
 * 为什么是「定点」而不是「第几个 tick」
 * ============================================================
 *
 * 一个自然的想法是把价格存成「第几个 tick」的整数。这里没有那么做，三个理由：
 *
 * 1. **Session 根本没有 tick_size 这个成员** —— 它是 seed_orders 的一个逐次
 *    传入的参数，从不保存。也就是说没有一个权威的 tick 可供换算；真要那么做，
 *    还得先把 tick 塞进 Session，而两个 tick 不同的 session 会产出互不可比、
 *    类型系统又区分不了的 Ticks 值。
 *
 * 2. 固定标度让 Price **自描述**：任何两个 Price 都可比，可以 constexpr 构造，
 *    而 tick_size 回归它本来的角色 —— 一条**量化/校验规则**（「价格必须是 N 的整数倍」），
 *    而不是表示单位。
 *
 * 3. **往返是可证的，不是碰运气的**：`raw / 10000.0` 是一个可精确表示的整数
 *    除以一个可精确表示的整数，IEEE 754 保证结果是最接近该十进制值的 double ——
 *    与 strtod 解析同一个字面量得到的位模式完全相同。而「往返不一致」正是
 *    上面那个缺陷的根源。
 *
 * 标度取 1e-4（kScale = 10000）：int64 在这个标度下可表示 ±9.2e14，
 * 对本项目的 .2f 形态数据绰绰有余。加密货币若要更细可以调大标度，
 * 那是改一个常量的事。
 *
 * ============================================================
 * 为什么是强类型而不是裸 int64_t
 * ============================================================
 *
 * 要消灭的这一类缺陷，本质是「一个数被当成了另一种含义的数」。
 * 裸 int64_t 的价格与 quantity（整型）、timestamp（int64_t）可以随意互相赋值，
 * 编译器一声不吭。包成类之后：
 *   - `order.price = order.quantity` 编译不过
 *   - `price * quantity`（定点数下没有重新标定就是错的）编译不过
 * 代价只是一点样板代码。
 */

#ifndef ORDERBOOK_PRICE_H
#define ORDERBOOK_PRICE_H

#include <cmath>
#include <cstdint>
#include <ostream>
#include <string>

namespace orderbook {

class Price {
public:
    /// 标度：1 个 raw 单位 = 1e-4 报价货币单位
    static constexpr std::int64_t kScale = 10000;

    constexpr Price() = default;

    /// 从原始定点整数构造。命名构造，避免与「从一个数值构造」混淆。
    static constexpr Price from_raw(std::int64_t raw) { return Price(raw); }

    /// 从 double 构造（只应在 JSON 边界使用）
    static Price from_double(double d) {
        return Price(static_cast<std::int64_t>(std::llround(d * static_cast<double>(kScale))));
    }

    constexpr std::int64_t raw() const { return raw_; }

    /// 转回 double（只应在 JSON 边界使用）
    ///
    /// 这一步是**无损**的：分子分母都可精确表示，IEEE 754 的除法给出最接近
    /// 真实十进制值的 double —— 与直接解析同一个字面量得到的位模式一致。
    constexpr double to_double() const {
        return static_cast<double>(raw_) / static_cast<double>(kScale);
    }

    /// 是否是某个 tick 的整数倍（tick 也用 raw 单位表示）
    constexpr bool is_multiple_of(std::int64_t tick_raw) const {
        return tick_raw > 0 && (raw_ % tick_raw) == 0;
    }

    // ── 比较：全部是精确的整数比较 ──
    friend constexpr bool operator==(Price a, Price b) { return a.raw_ == b.raw_; }
    friend constexpr bool operator!=(Price a, Price b) { return a.raw_ != b.raw_; }
    friend constexpr bool operator<(Price a, Price b) { return a.raw_ < b.raw_; }
    friend constexpr bool operator>(Price a, Price b) { return a.raw_ > b.raw_; }
    friend constexpr bool operator<=(Price a, Price b) { return a.raw_ <= b.raw_; }
    friend constexpr bool operator>=(Price a, Price b) { return a.raw_ >= b.raw_; }

    // ── 算术：只开放有意义的那几种 ──
    // 刻意**不**提供 Price*Price 与 Price/Price：定点数下它们不重新标定就是错的。
    friend constexpr Price operator+(Price a, Price b) { return Price(a.raw_ + b.raw_); }
    friend constexpr Price operator-(Price a, Price b) { return Price(a.raw_ - b.raw_); }
    friend constexpr Price operator*(Price a, std::int64_t k) { return Price(a.raw_ * k); }
    friend constexpr Price operator*(std::int64_t k, Price a) { return Price(a.raw_ * k); }

    /// 给 GoogleTest 用：失败信息里打人可读的价格而不是字节堆
    friend std::ostream& operator<<(std::ostream& os, Price p) {
        return os << p.to_double() << " (raw " << p.raw_ << ")";
    }

private:
    explicit constexpr Price(std::int64_t raw) : raw_(raw) {}
    std::int64_t raw_ = 0;
};

/*
 * tick 大小 —— 一条**规则**，不是一个价格。
 *
 * 单独成一个类型，是为了让 `seed_orders(count, mid, tick, ...)` 这类调用里
 * 价格和 tick 不会被互相传错（两者都是 int64 的话就没有这层保护）。
 */
struct TickSize {
    std::int64_t raw = Price::kScale / 100;   // 默认 0.01

    static TickSize from_double(double d) {
        return TickSize{static_cast<std::int64_t>(
            std::llround(d * static_cast<double>(Price::kScale)))};
    }
    constexpr double to_double() const {
        return static_cast<double>(raw) / static_cast<double>(Price::kScale);
    }
};

}  // namespace orderbook

#endif  // ORDERBOOK_PRICE_H
