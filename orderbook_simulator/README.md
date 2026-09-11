# orderbook_simulator

A limit order book and matching engine simulator in C++17, with a built-in REST API server
(default `:8001`).

*[中文版 / Chinese version](README.zh-CN.md)*

**~3.2k lines of C++ · four order types · 42 GoogleTest cases green · `-Wall -Wextra -Werror` clean**

---

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

./build/orderbook_tests          # unit tests
./build/orderbook_server         # start server (default :8001)
./build/orderbook_server 9001    # pick a port
```

## What the book looks like

```
    asks (low to high, price priority)
      102.00  ├─ 200 shares  (2 orders)
      101.50  ├─ 300 shares  (3 orders)
      101.00  ├─ 150 shares  (1 order)   ← best_ask
    ──────────┼──────── spread = 0.50, mid = 100.75 ────────
      100.50  ├─ 180 shares  (2 orders)  ← best_bid
      100.00  ├─ 400 shares  (5 orders)
       99.50  ├─ 250 shares  (3 orders)
    bids (high to low, price priority)

    Each level is a FIFO queue internally (time priority):
      100.00 ─► [A 100sh] ─► [B 200sh] ─► [C 100sh]
                    ↑ fills first
```

## Architecture

| File | Responsibility |
|---|---|
| `include/orderbook/types.h` | `Side` / `OrderType` / `BookOrder` / `Fill`, nanosecond timestamps and ID generation |
| `price_level.h` `.cpp` | FIFO order queue for one price level — the carrier of time priority. Level aggregates are maintained incrementally, so `is_empty()` / `order_count()` / `total_quantity()` are O(1) |
| `limit_order_book.h` `.cpp` | Two-sided price tree: `std::map<double, PriceLevel, std::greater<>>` for bids (descending), `std::map` for asks (ascending). Plus an `order_id → (side, price)` hash index, making cancels O(1). `best_bid`/`best_ask` are O(1) |
| `matching_engine.h` `.cpp` | Matching core: price priority then time priority, with the distinct semantics of all four order types |
| `market_impact.h` | Market impact model (square-root law): estimates slippage for large orders by participation rate |
| `statistics.h` `.cpp` | Book statistics: spread, relative spread (bps), depth, imbalance, VWAP |
| `session.h` `.cpp` | One independent experiment: its own book, matching engine and fill log. Can be seeded with a random-but-reproducible book |
| `session_manager.h` `.cpp` | Session isolation, `unordered_map<string, unique_ptr<Session>>` |
| `server.h` `.cpp` | REST API routing |

---

## Known issues

All measured, and written down here rather than hidden.

### Floating-point prices as ordered map keys — this breaks time priority

The book is a `std::map<double, PriceLevel>`. Two algebraically equal formulas give
different doubles:

```
100.00 + 7*0.01                      = 100.06999999999999
round((100.00 + 7*0.01)/0.01)*0.01   = 100.07000000000001   ← the form Session::seed_orders uses
```

So "the 100.07 level" becomes **two distinct keys**. Two consequences:

- `bid_quantity_at(100.07)` reports only one of the two levels' quantity;
- **time priority is broken**: two orders at the same nominal price sit on different levels,
  and matching picks the "best" level by double comparison — so **a later order can fill
  before an earlier one**. Measured: `FIRST` (97 shares, ts=1) is skipped entirely while
  `SECOND` (417 shares, ts=2) absorbs all 153 shares.

This is not a rounding blemish. The "Order types" section below promises *price priority, then
time priority (FIFO within a level)*, and that promise is violated.

**This defect was found by the randomized differential test**, not by reading the code
([`tests/test_differential.cpp`](tests/test_differential.cpp), seed 12648430, auto-shrunk
from 300 steps to 3). The reference model keys prices by integer tick, so it structurally
cannot split a level; the real book did.

Acceptance tests live in [`tests/test_price_integrity.cpp`](tests/test_price_integrity.cpp),
currently marked `DISABLED_` — a permanently red CI is the same as no CI, so a known defect
gets a disabled test plus documentation instead of a red badge:

```bash
./build/orderbook_tests --gtest_also_run_disabled_tests --gtest_filter='PriceIntegrity*'
```

Planned fix: a strong `int64` fixed-point price type, with double↔fixed-point conversion
confined to the JSON boundary.

---

## Fixed: two complexity problems

Both were in "Known issues" until recently. Kept on record because the process is more
interesting than the result.

### 1. `cancel_order` used to be a full-book linear scan

With no `order_id` index, a cancel walked every bid level, scanned each level's whole FIFO
queue, then did the same for asks. Cancelling a non-existent id was always the worst case —
both sides scanned in full.

The fix is what production LOBs do: an `order_id → (side, price)` hash table.

| Metric | Before | After | Change |
|---|---|---|---|
| **Cancel, amortized** | 44,389 ns | **142 ns** | **312×** |
| Cancel p50 | 42,208 ns | 84 ns ⚠️ | 502× |
| Cancel p99.9 | 126,333 ns | 792 ns ⚠️ | —— |
| Cancel / submit p50 | 48.2× | 0.13× | —— |

⚠️ **The headline number is the amortized one, not p50.** After the fix a cancel takes
2.0 clock ticks (measured `steady_clock` granularity on this machine: 41 ns), so a single
measurement carries roughly ±24% quantization error — the trailing digits on those rows are
artifacts, not signal. Before the fix, p50 was 1,029 ticks and entirely trustworthy; after,
it is not. So the defensible speedup is the amortized **312×**, not the 502× that p50 implies.
The larger number looks better, but part of it comes from *the thing under test becoming too
fast to measure*.

Both datasets were produced by the **same** benchmark binary (both with warmup, both measuring
clock granularity); the "before" row comes from running today's `bench_orderbook.cpp` against
the pre-fix commit. A before/after comparison across different harnesses is not a comparison.

### 2. `is_empty()` / `order_count()` / `total_quantity()` used to be O(n)

Because cancellation is soft (it sets `is_active = false` without dequeuing), all three had to
scan past the "corpses" in the queue to answer. And `is_empty()` is called by `best_bid()`,
`best_ask()`, `get_depth()` and `cleanup()` — which is exactly why this README used to claim
"best bid/ask is O(1)" when it wasn't.

Replaced with two incrementally maintained counters (active order count, active quantity).

(Amortized values below as well: a single `get_depth` is only 2–3 clock ticks, so its p50 is
quantization-limited too.)

| Metric (amortized) | Before | After |
|---|---|---|
| `get_depth(10)` @ 1,000 orders | 215 ns | 149 ns |
| `get_depth(10)` @ 10,000 orders | 242 ns | 147 ns |
| `get_depth(10)` @ 100,000 orders | 585 ns | 151 ns |
| Submit p50 (15 ticks, not limited) | 875 ns | 626 ns |

The point is not the smaller constant, it's the **changed order**: before, `get_depth` grew
from 215 to 585 ns with book depth; after, it is essentially flat (149 / 147 / 151).

Submit got faster too, despite now doing an extra index insertion — because every submit calls
`cleanup()`, which calls `is_empty()` on every level, and the saving exceeds the new cost.

**Behavioral equivalence for both changes is guarded by the randomized differential test**
([`tests/test_differential.cpp`](tests/test_differential.cpp): 200 seeds × 300 steps =
60,000 operations, fill-for-fill identical to the reference model). Guardrail first, then
performance — not the other way around.

---

## Order types

| Type | Semantics |
|---|---|
| `MARKET` | No price limit; consumes the opposite side until filled or the book is empty |
| `LIMIT` | Fills only at the given price or better; the remainder rests on the book |
| `IOC` | Immediate or Cancel: fill what you can, cancel the rest immediately, **never rests** |
| `FOK` | Fill or Kill: **pre-checks total available quantity**; rejects the whole order if short (zero shares fill), matches only if sufficient |

> FOK genuinely performs an all-or-nothing **pre-check** (`available_quantity()`) rather than
> matching and rolling back. A rollback implementation leaves already-created fill records
> behind mid-flight, which is the wrong semantics.

---

## REST API

### `GET /health`

```bash
curl http://127.0.0.1:8001/health
```

### `POST /api/sessions` — create a session

```jsonc
{
  "symbol": "TEST",       // default "TEST"
  "mid_price": 100.0,     // mid price used for seeding
  "seed_count": 200,      // how many initial orders to seed (0 = empty book)
  "tick_size": 0.01,      // minimum price increment
  "spread_ticks": 2       // initial spread, in ticks
}
```

→ `201 Created`

```json
{ "session_id": "...", "symbol": "TEST", "seed_count": 200, "message": "Session created successfully" }
```

### `GET /api/sessions/{id}/depth?levels=10` — book depth

```json
{
  "bids": [{ "price": 100.50, "quantity": 180, "order_count": 2 }],
  "asks": [{ "price": 101.00, "quantity": 150, "order_count": 1 }],
  "spread": 0.50,
  "mid_price": 100.75
}
```

### `POST /api/sessions/{id}/orders` — submit an order

```jsonc
{
  "side": "BUY",           // BUY | SELL
  "order_type": "LIMIT",   // MARKET | LIMIT | IOC | FOK
  "price": 101.00,         // omit for MARKET
  "quantity": 500,
  "client_tag": "user"     // "user" for user orders, "mm" for market-maker orders
}
```

→ returns the matching result:

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

### `DELETE /api/sessions/{id}/orders/{order_id}` — cancel

```json
{ "success": true, "order_id": "...", "message": "Order cancelled" }
```

### `GET /api/sessions/{id}/fills` — fill log

### `GET /api/sessions/{id}/stats` — book statistics

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

`imbalance = (bid_qty − ask_qty) / (bid_qty + ask_qty)`, in `[-1, 1]` — the most basic
short-horizon directional microstructure signal.

### `POST /api/sessions/{id}/seed` — seed additional orders

```jsonc
{ "count": 100, "mid_price": 100.0, "tick_size": 0.01, "spread_ticks": 2 }
```

---

## Market impact model

`MarketImpact::estimate_impact(volatility, trade_quantity, daily_volume)` estimates how far a
large order moves the price, as a function of participation rate
(`trade_quantity / daily_volume`).

The intuition: you want 10,000 shares, but the ask side has only 2,000 at `101.00`,
3,000 at `101.50` and 5,000 at `102.00` — your order sweeps all three levels for an average
around `101.55`. Buy 100 shares instead and you pay `101.00`. Impact grows **non-linearly**
with participation rate, which is one reason backtests that fill at the closing price
systematically overstate strategy returns.

---

## Testing

```bash
./build/orderbook_tests                              # 42 cases
./build/orderbook_tests --gtest_filter='Differential*'   # randomized differential test
OB_SOAK_SEEDS=2000 ./build/orderbook_tests --gtest_filter='Differential*'   # soak run
./build/orderbook_tests --gtest_also_run_disabled_tests --gtest_filter='PriceIntegrity*'
```

The differential test replays the same random order flow against the real engine and a
deliberately slow, obviously correct reference model — a single `std::vector` scanned linearly
for the best-priced, earliest-inserted counterparty. O(n²), but its correctness is visible
rather than argued, which is the only property a reference model needs.

Every step compares the fill sequence (price, quantity, aggressor, **order**) and the full
resulting book state, then checks invariants (book uncrossed, level totals equal the sum of
active remainders). Comparing only fills would miss "fills were right but residual state was
wrong" — a class of bug that surfaces several operations later, when it is much harder to
locate. On failure the script is shrunk (binary tail truncation plus greedy single-op removal)
to a minimal reproduction.
