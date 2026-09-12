/*
 * test_order_book.cpp — LimitOrderBook 的单元测试
 */

#include <gtest/gtest.h>
#include "orderbook/limit_order_book.h"
#include "test_price_helpers.h"

using namespace orderbook;

// 辅助函数：创建一个买单
static BookOrder make_bid(const std::string& id, double price, int qty) {
    BookOrder o;
    o.order_id = id;
    o.side = Side::BUY;
    o.order_type = OrderType::LIMIT;
    o.price = P(price);   // 辅助函数内部做一次转换，调用点保留十进制字面量
    o.quantity = qty;
    o.timestamp = now_ns();
    return o;
}

// 辅助函数：创建一个卖单
static BookOrder make_ask(const std::string& id, double price, int qty) {
    BookOrder o;
    o.order_id = id;
    o.side = Side::SELL;
    o.order_type = OrderType::LIMIT;
    o.price = P(price);   // 辅助函数内部做一次转换，调用点保留十进制字面量
    o.quantity = qty;
    o.timestamp = now_ns();
    return o;
}


TEST(OrderBookTest, EmptyBook) {
    LimitOrderBook book;

    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.spread().has_value());
    EXPECT_FALSE(book.mid_price().has_value());
}

TEST(OrderBookTest, SingleBid) {
    LimitOrderBook book;
    book.add_order(make_bid("b1", 100.0, 500));

    EXPECT_EQ(book.best_bid().value(), P(100.0));
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.bid_quantity_at(P(100.0)), 500);
}

TEST(OrderBookTest, SingleAsk) {
    LimitOrderBook book;
    book.add_order(make_ask("a1", 101.0, 300));

    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.best_ask().value(), P(101.0));
}

TEST(OrderBookTest, BidAskSpread) {
    LimitOrderBook book;
    book.add_order(make_bid("b1", 100.0, 500));
    book.add_order(make_ask("a1", 101.0, 300));

    EXPECT_EQ(book.best_bid().value(), P(100.0));
    EXPECT_EQ(book.best_ask().value(), P(101.0));
    // spread 现在是精确的 Price，可以用整数相等比较（原来是浮点比较）
    EXPECT_EQ(book.spread().value(), P(1.0));
    EXPECT_DOUBLE_EQ(book.mid_price().value(), 100.5);
}

TEST(OrderBookTest, BidsSortedDescending) {
    // 买盘应该从高到低排列
    LimitOrderBook book;
    book.add_order(make_bid("b1", 99.0, 100));
    book.add_order(make_bid("b2", 100.0, 200));
    book.add_order(make_bid("b3", 98.0, 300));

    EXPECT_EQ(book.best_bid().value(), P(100.0));  // 最高买价 = 100.0

    auto depth = book.get_depth(10);
    EXPECT_EQ(depth.bids.size(), 3);
    EXPECT_EQ(depth.bids[0].price, P(100.0));  // 第一档
    EXPECT_EQ(depth.bids[1].price, P(99.0));   // 第二档
    EXPECT_EQ(depth.bids[2].price, P(98.0));   // 第三档
}

TEST(OrderBookTest, AsksSortedAscending) {
    // 卖盘应该从低到高排列
    LimitOrderBook book;
    book.add_order(make_ask("a1", 103.0, 100));
    book.add_order(make_ask("a2", 101.0, 200));
    book.add_order(make_ask("a3", 102.0, 300));

    EXPECT_EQ(book.best_ask().value(), P(101.0));  // 最低卖价 = 101.0

    auto depth = book.get_depth(10);
    EXPECT_EQ(depth.asks.size(), 3);
    EXPECT_EQ(depth.asks[0].price, P(101.0));
    EXPECT_EQ(depth.asks[1].price, P(102.0));
    EXPECT_EQ(depth.asks[2].price, P(103.0));
}

TEST(OrderBookTest, MultipleBidsAtSamePrice) {
    // 同一价格上多个订单
    LimitOrderBook book;
    book.add_order(make_bid("b1", 100.0, 200));
    book.add_order(make_bid("b2", 100.0, 300));

    EXPECT_EQ(book.bid_quantity_at(P(100.0)), 500);   // 200 + 300

    auto depth = book.get_depth(10);
    EXPECT_EQ(depth.bids.size(), 1);          // 只有一档
    EXPECT_EQ(depth.bids[0].quantity, 500);
    EXPECT_EQ(depth.bids[0].order_count, 2);  // 两个订单
}

TEST(OrderBookTest, CancelOrder) {
    LimitOrderBook book;
    book.add_order(make_bid("b1", 100.0, 500));

    EXPECT_TRUE(book.cancel_order("b1"));
    EXPECT_EQ(book.bid_quantity_at(P(100.0)), 0);
}

TEST(OrderBookTest, CancelNonexistent) {
    LimitOrderBook book;
    book.add_order(make_bid("b1", 100.0, 500));

    EXPECT_FALSE(book.cancel_order("nonexistent"));
    EXPECT_EQ(book.bid_quantity_at(P(100.0)), 500);
}

TEST(OrderBookTest, FindOrder) {
    LimitOrderBook book;
    book.add_order(make_bid("b1", 100.0, 500));
    book.add_order(make_ask("a1", 101.0, 300));

    auto found_bid = book.find_order("b1");
    EXPECT_TRUE(found_bid.has_value());
    EXPECT_EQ(found_bid->price, P(100.0));

    auto found_ask = book.find_order("a1");
    EXPECT_TRUE(found_ask.has_value());
    EXPECT_EQ(found_ask->price, P(101.0));

    auto not_found = book.find_order("xxx");
    EXPECT_FALSE(not_found.has_value());
}

TEST(OrderBookTest, DepthLevelsLimit) {
    LimitOrderBook book;
    // 添加 5 档买盘
    for (int i = 0; i < 5; i++) {
        book.add_order(make_bid("b" + std::to_string(i), 100.0 - i, 100));
    }

    // 只取 3 档
    auto depth = book.get_depth(3);
    EXPECT_EQ(depth.bids.size(), 3);
    EXPECT_EQ(depth.bids[0].price, P(100.0));   // 最高价在前
    EXPECT_EQ(depth.bids[2].price, P(98.0));
}
