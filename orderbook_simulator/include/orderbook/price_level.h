/*
 * ============================================================
 * price_level.h — 单价位 FIFO 订单队列
 * ============================================================
 *
 * 什么是 PriceLevel？
 *
 * 在订单簿里，同一个价格上可能有很多人挂单。
 * 比如 100.00 元这个价格上：
 *   张三挂了 100 股（先来的）
 *   李四挂了 200 股（后来的）
 *   王五挂了  50 股（最后的）
 *
 * PriceLevel 就是管理"同一个价格上的所有订单"的容器。
 * 它按照 FIFO（先进先出 / 先来后到）的顺序处理订单：
 * 有人来买时，先吃张三的，再吃李四的，最后吃王五的。
 *
 * 数据结构选择：std::deque（双端队列）
 *   - 头部弹出 O(1) — 撮合时从前面吃
 *   - 尾部插入 O(1) — 新订单从后面排队
 *   - 比 std::list 内存更紧凑（缓存友好）
 *   - 比 std::vector 头部弹出更快（vector 头部弹出要移动所有元素）
 *
 * ============================================================
 */

#ifndef ORDERBOOK_PRICE_LEVEL_H
#define ORDERBOOK_PRICE_LEVEL_H

#include <deque>       // 双端队列
#include <vector>
#include <string>
#include "orderbook/types.h"

namespace orderbook {

class PriceLevel {
public:
    // ── 构造函数 ──
    // explicit 关键字：防止编译器做隐式类型转换
    // 没有 explicit 的话，PriceLevel level = 100.0; 这种写法会编译通过
    // 加了 explicit 后，必须写 PriceLevel level(100.0); 更安全
    explicit PriceLevel(Price price);

    // ── 查询方法（都是 const，不修改对象）──

    /// 这个价位是多少钱
    Price price() const;

    /// 该价位上所有活跃订单的总量
    ///
    /// O(1)：由增量维护的计数器直接返回。
    /// 原来是每次调用都遍历整条 deque 求和——而 get_depth 对每档要调
    /// total_quantity() + order_count() 两次，再加 is_empty()，
    /// 一次取盘口就把整个簿扫了好几遍。
    int total_quantity() const;

    /// 该价位上有多少个活跃订单（O(1)）
    int order_count() const;

    /// 是否没有活跃订单了
    ///
    /// O(1)。这一条尤其要紧：is_empty() 被 best_bid()/best_ask()/get_depth()/
    /// best_bid_level()/best_ask_level()/cleanup() 全都调用，而它原来是 O(n)
    /// ——因为撤单是软撤单（只置 is_active=false，不出队），必须扫过队列里的
    /// 「尸体」才能确定还有没有活跃单。这就是 README 原先声称
    /// 「取 best bid/ask 是 O(1)」却不成立的原因。
    bool is_empty() const;

    /// 获取所有活跃订单的列表（返回拷贝，不影响内部状态）
    std::vector<BookOrder> get_orders() const;

    // ── 修改方法 ──

    /// 添加一个新订单到队尾（排队）
    void add_order(BookOrder order);
    // 注意这里参数不是引用（BookOrder order 而不是 BookOrder& order）
    // 这是因为我们要把订单"移入"队列，传值后可以 std::move

    /// 撤销指定订单，返回是否撤销成功
    bool remove_order(const std::string& order_id);

    /// 撮合：用来单的数量去吃这个价位上的订单
    /// 参数：
    ///   incoming_qty      — 来单想成交的数量
    ///   aggressor_side    — 来单的方向（谁主动发起的交易）
    ///   aggressor_order_id — 来单的订单 ID
    /// 返回值：
    ///   pair.first  — 实际成交了多少股
    ///   pair.second — 成交记录列表
    /// 　　retired_ids — 可选的输出参数。本次撮合中**彻底离开簿**
    ///                   （全部成交）的挂单 id 会追加到这里。
    ///                   LimitOrderBook 用它把 order_id 索引里的条目清掉，
    ///                   否则索引会随着成交单不断累积陈旧条目。
    ///                   传 nullptr 表示不关心（现存测试就是这么调的）。
    std::pair<int, std::vector<Fill>> match(
        int incoming_qty,
        Side aggressor_side,
        const std::string& aggressor_order_id,
        std::vector<std::string>* retired_ids = nullptr
    );

private:
    Price price_;                      // 这个价位的价格（定点，见 price.h）
    std::deque<BookOrder> orders_;     // 订单队列（FIFO）

    /*
     * ── 增量维护的聚合量 ──
     *
     * 这两个计数器把 is_empty()/order_count()/total_quantity() 从 O(n) 变成 O(1)。
     *
     * 不变量（每次修改队列后都必须成立，差分测试会验证）：
     *   active_count_    == 队列中 is_active && remaining() > 0 的订单数
     *   active_quantity_ == 上述订单的 remaining() 之和
     *
     * 之所以能安全地缓存，是因为队列的每一处改动都收在本类的四个方法里：
     * add_order / remove_order / match / （构造）。外部拿不到可写的队列引用。
     */
    int active_count_ = 0;
    long long active_quantity_ = 0;

    // 命名约定：成员变量以下划线结尾（price_、orders_）
    // 这是 Google C++ 代码风格的惯例，方便区分成员变量和局部变量
};

}  // namespace orderbook

#endif // ORDERBOOK_PRICE_LEVEL_H
