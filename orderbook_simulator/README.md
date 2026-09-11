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
| `limit_order_book.h` `.cpp` | 双向价格树：`std::map<double, PriceLevel, std::greater<>>` 存买盘（降序）、`std::map` 存卖盘（升序）。取 best bid/ask 是 O(1)（`begin()` + O(1) 的空档判断，且撤单会就地删掉空档） |
| `matching_engine.h` `.cpp` | 撮合核心：价格优先 + 时间优先，四种订单类型各自的语义 |
| `market_impact.h` | 市场冲击模型（平方根律）：按参与率估算大单的滑价 |
| `statistics.h` `.cpp` | 盘口统计：价差、相对价差（bps）、深度、买卖失衡、VWAP |
| `session.h` `.cpp` | 一次独立实验：自带订单簿 + 撮合引擎 + 成交流水，可随机播种模拟真实盘口 |
| `session_manager.h` `.cpp` | 多会话隔离，`unordered_map<string, unique_ptr<Session>>` |
| `server.h` `.cpp` | REST API 路由 |

### 已知问题

都是实测出来的，写在这里而不是藏起来。

**0. 浮点价格被当作有序 map 的 key —— 会破坏时间优先（最严重）**

订单簿是 `std::map<double, PriceLevel>`。两个代数上相等的算式给出不同的 double：

```
100.00 + 7*0.01                      = 100.06999999999999
round((100.00 + 7*0.01)/0.01)*0.01   = 100.07000000000001   ← seed_orders 用的写法
```

于是「100.07 这一档」在簿里成了**两个不同的 key**。两个后果：

- `bid_quantity_at(100.07)` 只报其中一档的量；
- **时间优先被破坏**：同一名义价格上的两单落在不同档位，撮合按 double 大小取
  「最优」档，于是**后到的订单可能先成交，先到的被跳过**。实测 FIRST(97股,
  ts=1) 被跳过、SECOND(417股, ts=2) 直接吃掉 153 股。

这不是精度瑕疵——上面「订单类型」一节承诺的「时间优先（同价位 FIFO）」被违反了。

这个缺陷是 [`tests/test_differential.cpp`](tests/test_differential.cpp) 的随机化
差分测试**自己发现的**（种子 12648430，300 步自动收缩到 3 步），不是人读代码读出来的。
验收测试见 [`tests/test_price_integrity.cpp`](tests/test_price_integrity.cpp)，
当前以 `DISABLED_` 挂着：

```bash
./build/orderbook_tests --gtest_also_run_disabled_tests --gtest_filter='PriceIntegrity*'
```

修复方向：价格改成 int64 定点的强类型，map 的 key 变整数，
double↔定点的转换只发生在 JSON 边界。

---

### 已修复：两处复杂度问题

这两条原本也在「已知问题」里，现在修掉了。留着记录，因为过程比结论有意思。

**1. `cancel_order` 原来是全簿线性扫描。**

没有 `order_id` 索引，撤单要遍历买盘所有档、每档再遍历整条 FIFO 队列，
找不到再遍历卖盘。撤一个不存在的 id 永远是最坏情况——两边都扫完。

改法是生产级 LOB 的标准做法：一张 `order_id → (方向, 价位)` 的哈希表。

| 指标 | 改前 | 改后 | 变化 |
|---|---|---|---|
| **撤单摊销** | 44,389 ns | **142 ns** | **312×** |
| 撤单 p50 | 42,208 ns | 84 ns ⚠️ | 502× |
| 撤单 p99.9 | 126,333 ns | 792 ns ⚠️ | —— |
| 撤单 / 下单 p50 | 48.2× | 0.13× | —— |

⚠️ **头条数字用摊销值，不用 p50。** 改完之后撤单只有 2.0 个时钟 tick
（本机 `steady_clock` 粒度实测 41 ns），单次测量的量化相对误差约 ±24%，
那几行的尾数是量化产物而非信号。改之前 p50 有 1,029 个 tick、完全可信，
改之后不再可信——所以可信的倍数是摊销值的 **312×**，
而不是 p50 算出来的 502×。后者好看，但一部分来自「被测对象快到测不准了」。

两份数据用的是**同一版** benchmark 程序（都带预热、都实测时钟粒度）；
改前那份是把当前的 `bench_orderbook.cpp` 拿到改动前的提交上跑出来的。
harness 不同的 before/after 不可比。

**2. `is_empty()` / `order_count()` / `total_quantity()` 原来都是 O(n)。**

因为撤单是软撤单（只置 `is_active=false`，不出队），这三个方法必须扫过队列里的
「尸体」才能得出答案。而 `is_empty()` 被 `best_bid()`、`best_ask()`、`get_depth()`、
`cleanup()` 全都调用——这就是本 README 原先声称「取 best bid/ask 是 O(1)」
却不成立的原因。

改成两个增量维护的计数器（活跃单数、活跃总量）之后：

（下面同样用摊销值：`get_depth` 单次只有 2~3 个时钟 tick，p50 也已量化受限。）

| 指标（摊销） | 改前 | 改后 |
|---|---|---|
| `get_depth(10)` @ 簿深 1,000 | 215 ns | 149 ns |
| `get_depth(10)` @ 簿深 10,000 | 242 ns | 147 ns |
| `get_depth(10)` @ 簿深 100,000 | 585 ns | 151 ns |
| 下单 p50（15 个 tick，未受限） | 875 ns | 626 ns |

重点不在常数变小，在**阶数变了**：改前 `get_depth` 随簿深从 215 涨到 585 ns，
改后基本不随簿深变化（149 / 147 / 151）。

下单也变快了，尽管它多了一次索引插入——因为每次提交都会调 `cleanup()`，
而 `cleanup()` 要对每档调 `is_empty()`，省下的比新增的多。

**两处改动的行为等价由随机化差分测试守着**
（[`tests/test_differential.cpp`](tests/test_differential.cpp)，
200 个种子 × 300 步 = 6 万次操作，与参照模型逐笔一致）。
先有护栏、再改性能，顺序不能反。


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
