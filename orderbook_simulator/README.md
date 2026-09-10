# orderbook_simulator

限价订单簿（Limit Order Book）与撮合引擎模拟器，C++17 编写，自带 REST API 服务器（默认 `:8001`）。

**约 3.2k 行 C++ · 四种订单类型 · 40 个 GoogleTest 用例全绿 · `-Wall -Wextra -Werror` 零警告**

---

## 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

./build/orderbook_tests          # 跑单元测试
./build/orderbook_server         # 启动服务器（默认 8001）
./build/orderbook_server 9001    # 指定端口
```

## 订单簿长什么样

```
    卖盘 asks（从低到高，价格优先）
      102.00  ├─ 200 股  (2 单)
      101.50  ├─ 300 股  (3 单)
      101.00  ├─ 150 股  (1 单)   ← best_ask
    ──────────┼──────── spread = 0.50, mid = 100.75 ────────
      100.50  ├─ 180 股  (2 单)   ← best_bid
      100.00  ├─ 400 股  (5 单)
       99.50  ├─ 250 股  (3 单)
    买盘 bids（从高到低，价格优先）

    每个价位内部是一条 FIFO 队列（时间优先）：
      100.00 ─► [张三 100股] ─► [李四 200股] ─► [王五 100股]
                    ↑ 先成交
```

## 架构

| 文件 | 职责 |
|---|---|
| `include/orderbook/types.h` | `Side` / `OrderType` / `BookOrder` / `Fill`，纳秒时间戳与 ID 生成 |
| `price_level.h` `.cpp` | 单价位 FIFO 订单队列——同价位先来先成交的载体 |
| `limit_order_book.h` `.cpp` | 双向价格树：`std::map<double, PriceLevel, std::greater<>>` 存买盘（降序）、`std::map` 存卖盘（升序）。取 best bid/ask 是 `begin()` 起步的**跳空档扫描**，不是 O(1)——见下方「已知复杂度问题」 |
| `matching_engine.h` `.cpp` | 撮合核心：价格优先 + 时间优先，四种订单类型各自的语义 |
| `market_impact.h` | 市场冲击模型（平方根律）：按参与率估算大单的滑价 |
| `statistics.h` `.cpp` | 盘口统计：价差、相对价差（bps）、深度、买卖失衡、VWAP |
| `session.h` `.cpp` | 一次独立实验：自带订单簿 + 撮合引擎 + 成交流水，可随机播种模拟真实盘口 |
| `session_manager.h` `.cpp` | 多会话隔离，`unordered_map<string, unique_ptr<Session>>` |
| `server.h` `.cpp` | REST API 路由 |

### 已知复杂度问题

这两条是实测出来的，写在这里而不是藏起来：

**1. `best_bid()` / `best_ask()` 不是 O(1)。**
两个 `std::map` 取 `begin()` 确实是 O(1)，但实现要跳过已撤单的「尸体」——
`remove_order` 是软撤单，只把 `is_active` 置 false，不从 deque 里移除
（`price_level.cpp:91-101`）。所以 `best_bid()` 逐档调用 `is_empty()`，
而 `is_empty()` 自己要扫整条 deque 才能确定有没有活跃单
（`price_level.cpp:60-67`）。真实复杂度是 O(档数 × 档内单数)。

**2. `cancel_order` 是全簿线性扫描。**
没有 `order_id` 索引，撤单要遍历买盘所有档、再遍历卖盘所有档
（`limit_order_book.cpp:176-190`）。实测撤单 p50 是 42,500ns，
下单 p50 是 917ns —— **慢 46 倍**。撤一个不存在的 id 永远是最坏情况。

两条都在待修列表里（加活跃单计数 + `order_id → (Side, price)` 索引）。
在修好之前，README 不会声称它们是 O(1)。


## 订单类型

| 类型 | 语义 |
|---|---|
| `MARKET` | 市价单：不限价格，无条件吃对手盘直到成交完或对手盘吃空 |
| `LIMIT` | 限价单：只在指定价格或更优价格成交，未成交部分挂在簿上等待 |
| `IOC` | Immediate or Cancel：能成多少成多少，剩余部分立刻取消，**绝不挂单** |
| `FOK` | Fill or Kill：**先预检对手盘总量**，不够则整单拒绝（一股都不成交），够了才撮合 |

> FOK 是真的做「全有或全无」的**预检**（`available_quantity()`），而不是先撮合再回滚——回滚式实现会在中途留下已成交的 fill 记录，语义是错的。

## REST API

### `GET /health`

```bash
curl http://127.0.0.1:8001/health
```

### `POST /api/sessions` — 创建会话

```jsonc
{
  "symbol": "TEST",       // 默认 "TEST"
  "mid_price": 100.0,     // 播种时的中间价
  "seed_count": 200,      // 播种多少个初始订单（0 = 不播种，空簿）
  "tick_size": 0.01,      // 最小价格变动
  "spread_ticks": 2       // 初始价差（几个 tick）
}
```

→ `201 Created`

```json
{ "session_id": "...", "symbol": "TEST", "seed_count": 200, "message": "Session created successfully" }
```

### `GET /api/sessions/{id}/depth?levels=10` — 盘口深度

```json
{
  "bids": [{ "price": 100.50, "quantity": 180, "order_count": 2 }],
  "asks": [{ "price": 101.00, "quantity": 150, "order_count": 1 }],
  "spread": 0.50,
  "mid_price": 100.75
}
```

### `POST /api/sessions/{id}/orders` — 提交订单

```jsonc
{
  "side": "BUY",           // BUY | SELL
  "order_type": "LIMIT",   // MARKET | LIMIT | IOC | FOK
  "price": 101.00,         // 市价单可省略
  "quantity": 500,
  "client_tag": "user"     // 用户订单 "user"，做市商订单 "mm"
}
```

→ 返回撮合结果：

```json
{
  "order_id": "...",
  "filled_quantity": 350,
  "remaining_quantity": 150,
  "is_resting": true,
  "is_rejected": false,
  "fills": [{ "price": 101.00, "quantity": 150, "...": "..." }]
}
```

### `DELETE /api/sessions/{id}/orders/{order_id}` — 撤单

```json
{ "success": true, "order_id": "...", "message": "Order cancelled" }
```

### `GET /api/sessions/{id}/fills` — 成交流水

### `GET /api/sessions/{id}/stats` — 盘口统计

```json
{
  "spread": 0.50,
  "spread_bps": 49.6,
  "mid_price": 100.75,
  "bid_depth": 4300,
  "ask_depth": 3900,
  "imbalance": 0.049,
  "vwap": 100.82,
  "total_fills": 27,
  "total_volume": 5400
}
```

`imbalance = (买量 - 卖量) / (买量 + 卖量)`，范围 `[-1, 1]`，是最基础的短期方向性微观结构指标。

### `POST /api/sessions/{id}/seed` — 追加播种订单

```jsonc
{ "count": 100, "mid_price": 100.0, "tick_size": 0.01, "spread_ticks": 2 }
```

## 市场冲击模型

`MarketImpact::estimate_impact(volatility, trade_quantity, daily_volume)` 按参与率（`trade_quantity / daily_volume`）估算大单会把价格推动多少。

直觉：你想买 10000 股，但卖盘上 `101.00` 只有 2000 股、`101.50` 有 3000 股、`102.00` 有 5000 股——你的大单会把这三档全吃掉，均价约 `101.55`。而如果只买 100 股，成交价就是 `101.00`。冲击随参与率**非线性**增长，这就是回测里用收盘价成交会系统性高估策略收益的原因之一。
