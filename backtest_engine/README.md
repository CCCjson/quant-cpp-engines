# backtest_engine

An event-driven backtest engine for equities and crypto, in C++17, with a built-in REST API
server (default `:8002`).

*[中文版 / Chinese version](README.zh-CN.md)*

**~7.4k lines of C++ · 10 built-in strategies · 92 GoogleTest cases green · `-Wall -Wextra -Werror` clean**

---

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

./build/backtest_tests          # unit tests
./build/backtest_server         # start server (default :8002)
./build/backtest_server 9000    # pick a port
```

## Architecture

```
             ┌──────────────────────────────────┐
   HTTP ───► │  Server (cpp-httplib)            │
             │  /api/strategies                 │
             │  /api/backtest/run               │
             │  /api/backtest/run_signals       │
             │  /api/backtest/run_portfolio     │
             └────────────────┬─────────────────┘
                              │
             ┌────────────────▼─────────────────┐
             │  BacktestEngine                  │
             │  per-bar event loop:             │
             │    settle_t1() → mark_to_market  │
             │    → RiskManager stop-loss check │
             │    → strategy.on_bar(ctx)        │
             │    → orders queued to N+1 open   │
             └───┬──────────┬──────────┬────────┘
                 │          │          │
       ┌─────────▼──┐  ┌────▼─────┐  ┌─▼──────────┐
       │ IStrategy  │  │ Portfolio│  │ Metrics    │
       │ pure vtbl  │  │ pos/cash │  │ 19 metrics │
       │ 10 impls   │  │ T+1 lock │  │ sharpe/dd  │
       └────────────┘  └──────────┘  └────────────┘
```

| File | Responsibility |
|---|---|
| `include/backtest/types.h` | `Bar` / `Order` / `Fill` / `Position` / `EquitySnapshot` / `CommissionConfig` / `MarketRules` |
| `include/backtest/strategy_base.h` | `IStrategy` pure virtual base — `name()` / `description()` / `param_schema()` / `on_bar()` |
| `include/backtest/strategy_context.h` | Read-only context handed to strategies (bar history, current position, cash) |
| `src/engine.cpp` | Per-bar event loop, order execution, T+1 settlement |
| `src/portfolio.cpp` | Position management, cash tracking, sellable-quantity freeze/unfreeze |
| `src/risk_manager.cpp` | Fixed stop-loss, trailing stop, per-symbol position cap, total position cap |
| `src/metrics.cpp` | Returns, volatility, max drawdown, Sharpe, Sortino, win rate, profit factor |
| `src/data_loader.cpp` | JSON / CSV loading, plus synthetic data generation for tests |
| `src/server.cpp` | REST API routing |

---

## Built-in strategies

| name | Description | Key parameters |
|---|---|---|
| `MA_CROSS` | Moving-average cross: buy when fast crosses above slow, sell on cross below | `fast_period=5` `slow_period=20` |
| `MOMENTUM` | Buy when the N-day return exceeds a threshold, sell when it falls below one | `lookback=20` `buy_threshold=0.05` `sell_threshold=-0.03` |
| `MACD` | Buy when DIF crosses above DEA, sell on cross below | `fast_period=12` `slow_period=26` `signal_period=9` |
| `RSI` | Buy on recovery out of oversold, sell on decline out of overbought | `period=14` `oversold=30` `overbought=70` |
| `KDJ` | Buy when K crosses above D in the low zone, sell on cross below in the high zone | `n=9` `m1=3` `m2=3` `oversold=20` `overbought=80` |
| `BOLLINGER` | Buy on a bounce off the lower band, sell on a fade off the upper band | `period=20` `num_std=2.0` |
| `COMBO` | Weighted blend of sub-strategy signals, fires past a threshold | `threshold=0.5` `sub_strategies=[{name,weight,params}]` |
| `PAIRS` | Pairs trading (two legs, long-only rotation) driven by spread z-score | `symbol2` `lookback=60` `entry_z=2.0` `exit_z=0.5` |
| `SIGNAL` | External signal replay: fills a `[{date, action, price?, weight?}]` sequence | — (via `/run_signals`) |
| `PORTFOLIO_SIGNAL` | Portfolio signal replay: one signal sequence per symbol, **sharing one cash pool** | — (via `/run_portfolio`) |

All strategies share `position_pct` (fraction of capital per entry, default `0.95`).

> `GET /api/strategies` returns the **first 8**. `SIGNAL` and `PORTFOLIO_SIGNAL` are not
> selected via the `strategy` field — they are installed implicitly by the `/run_signals` and
> `/run_portfolio` endpoints, so they are not listed in the catalog.

### Fixed: KDJ's zone filter had its thresholds swapped

`KDJ`'s overbought/oversold filter used to have its two thresholds swapped relative to its own
documentation:

```cpp
// before
golden_cross = ... && (result.k < overbought_);   // k < 80, i.e. "not overbought"
death_cross  = ... && (result.k > oversold_);     // k > 20, i.e. "not oversold"
```

Because K spends most of its time inside (20, 80), both filters were near-always true, so the
strategy degenerated to a bare K/D cross with no zone filter — and the `oversold` /
`overbought` parameters were simultaneously *inverted in meaning* and *nearly inert*. Anyone
tuning them got the opposite of the documented effect.

Two things were fixed together:

1. The thresholds were swapped back, making the code agree with `description()`, the parameter
   schema, this README, and `benchmarks/python_reference`'s implementation.
2. An `initialized_` guard was added, as `MACDStrategy` already had. Without it, `prev_k_` and
   `prev_d_` both start at 50.0 while the `bar_index < n_` early return never updates them —
   so on the first evaluated bar `prev_k_ <= prev_d_` and `prev_k_ >= prev_d_` are *both* true,
   fabricating a cross against invented previous values.

**Confirmation that the attribution was right**: on the same synthetic data, C++ KDJ trade
counts went from 160 / 392 / 1,674 (at 1k / 2.5k / 10k bars) to **3 / 3 / 13** — and the Python
reference on that same data gives 3 / 3 / 13. A 53–131× workload divergence became an exact
match, which is strong evidence the divergence came from those two thresholds and nothing else.

`tests/test_kdj_strategy.cpp` adds five cases. Before this, `grep -riE "kdj" tests/` returned
nothing at all — the defect had no protection in either direction: nothing found it, and
nothing would have stopped it being reintroduced. Three of the five were verified to go red
while the defect was present (the other two cover intended behavior and the parameter schema).

One related issue is **not** fixed: `prev_k_` / `prev_d_` / `initialized_` are cross-bar state
held on the strategy object, while `BacktestEngine` shares one strategy instance across all
symbols in a portfolio backtest (see the warning at `engine.cpp:37-43`). Running KDJ
multi-symbol would let state bleed between symbols. Moving that state per-symbol is a
structural change and does not belong in a threshold fix; today no endpoint reaches that path
(`/run` installs a single-symbol strategy, `/run_portfolio` always installs
`PortfolioSignalStrategy`).

---

## Market presets

The `market` field selects commission, stamp duty, slippage and lot size. Four presets live in
factory methods in `types.h`:

| market | Commission | Stamp duty | Min commission | Default slippage | Lot |
|---|---|---|---|---|---|
| `a_share` (default) | 2.5 bps | 10 bps (sell only) | 5.0 | 0.1% | 100 |
| `us` | 1 bp | none | 1.0 | 0.05% | 1 |
| `hk` | 5 bps | 10 bps (both sides) | 5.0 | 0.1% | 1 |
| `crypto` | 10 bps (taker) | none | none | 0.05% | 1 |

`slippage_pct` can be overridden explicitly per request. In `risk_config`,
`max_position_pct` (per-symbol cap) and `max_total_position_pct` (total cap) are **two
independent constraints and the engine takes the stricter one** — taking the `max` would
silently disable a "keep 20% in cash" guard.

---

## REST API

### `GET /api/strategies`

Returns the strategy catalog and each one's parameter schema.

```bash
curl http://127.0.0.1:8002/api/strategies
```

### `POST /api/backtest/run` — single-symbol strategy backtest

```jsonc
{
  "symbol": "AAPL",
  "strategy": "MA_CROSS",
  "params": { "fast_period": 5, "slow_period": 20 },
  "bars": [{ "date": "2025-01-02", "open": 100, "high": 102, "low": 99, "close": 101, "volume": 1000000 }],
  "initial_capital": 100000,
  "market": "us",              // us | hk | a_share | crypto
  "slippage_pct": 0.001,       // optional; market default otherwise
  "risk_config": {             // optional
    "enabled": true,
    "stop_loss_pct": 0.05,
    "trailing_stop": true,
    "trailing_stop_pct": 0.08,
    "max_position_pct": 1.0,
    "max_total_position_pct": 0.8
  },
  "start_date": "2025-01-01",  // optional
  "end_date":   "2025-12-31"   // optional
}
```

> If `bars` is omitted, synthetic data is generated so you can try it quickly.

### `POST /api/backtest/run_signals` — external signal replay

Swaps the strategy for a signal sequence; execution, fees, slippage and T+1 all reuse the same
engine. `bars` is **required** here.

```jsonc
{
  "symbol": "AAPL",
  "signals": [{ "date": "2025-01-15", "action": "buy", "weight": 0.9, "price": 12.3 }],
  "bars": [ /* ... */ ],
  "initial_capital": 100000,
  "market": "us"
}
```

`action` is case-insensitive; `weight` defaults to `0.95`; omitting `price` means a market order.

### `POST /api/backtest/run_portfolio` — portfolio backtest

N symbols **share one cash pool**. A leg with `bars` but no `signals` is legal — that symbol
acts purely as market background (for instance the other leg of a pairs trade) and generates
no orders.

```jsonc
{
  "legs": [
    { "symbol": "BTCUSDT", "bars": [/*...*/], "signals": [/*...*/] },
    { "symbol": "ETHUSDT", "bars": [/*...*/], "signals": [/*...*/] }
  ],
  "initial_capital": 100000,
  "market": "crypto",
  "risk_config": { "enabled": true, "max_total_position_pct": 0.8 }
}
```

### Response shape (identical across all three endpoints)

```jsonc
{
  "symbol": "AAPL",
  "strategy_name": "MA_CROSS",
  "symbols": ["AAPL"],
  "metrics": {
    "total_return": 0.253, "annualized_return": 0.198, "final_value": 125300,
    "volatility": 0.21, "max_drawdown": 0.087, "max_drawdown_amount": 9400,
    "max_dd_start_date": "2025-03-11", "max_dd_end_date": "2025-04-02",
    "sharpe_ratio": 1.32, "sortino_ratio": 1.87,
    "total_trades": 24, "winning_trades": 15, "losing_trades": 9,
    "win_rate": 0.625, "profit_factor": 2.1,
    "avg_profit": 1830, "avg_loss": -870,
    "total_commission": 142.5, "total_slippage": 96.3
  },
  "equity_curve": [{ "date": "...", "cash": ..., "market_value": ..., "total_value": ..., "daily_return": ... }],
  "trades": [{ "order_id": "...", "symbol": "...", "side": "BUY", "price": ..., "quantity": ...,
               "commission": ..., "slippage": ..., "date": "...", "reason": "signal" }],

  // ── Nothing silent: these let the caller verify what the backtest actually did ──
  "dropped_last_bar_orders": 0,                       // orders dropped on the last bar for lack of a "next open"
  "bar_coverage": { "AAPL": 251 },                    // days each symbol actually had a bar (a missing bar is not a zero)
  "cash_contention": { "days": 3, "trimmed_notional": 8200.0 }  // proportional trimming from cash competition
}
```

`trades[].reason` is one of `signal` / `stop_loss` / `trailing_stop`.

> ⚠️ `metrics.total_trades` counts **completed round-trips** (one buy plus one sell), not the
> length of the `trades` array — a still-open final position is not counted.
> ⚠️ `profit_factor` is set to `0.0` when there are **zero losing trades** (divide-by-zero
> guard), which does not mean it is actually zero. Check `losing_trades` first.

---

## Deliberate design choices

**Signals fill at the next open.** A signal produced on bar N is always filled at bar N+1's
open. Filling today's signal at today's close is the most common source of look-ahead bias,
and it is blocked at the engine level rather than left to strategy authors.

**T+1 is modeled for real, not in a comment.** `Position` carries both `quantity` and
`available`. Shares bought today enter `quantity` but not `available`; `settle_t1()` unfreezes
them at the next open. Lot size is looked up per market too (`MarketRules.lot_size`) —
100 shares for A-shares, 1 unit for crypto.

**An empty date window returns 400 instead of running on everything.** If the caller's
`[start_date, end_date]` contains no bars at all, that is an **input** problem:
`EmptyDateRange` is thrown → HTTP 400. Silently running the full dataset and returning 200
would be lying to the caller.

**Portfolio backtests reject duplicate symbols.** `load_data` overwrites, so a silent collapse
would turn "a 10-symbol portfolio" into 3 while the returns still look perfectly normal.
Duplicate symbols therefore return 400.

**All three endpoints share one config parser.** There is exactly one `commission_of()` /
`risk_from_json()` / `market_rules_of()`. One endpoint once carried its own inlined copy, and
the result was that it silently ignored `max_total_position_pct` and never received
`MarketRules` — a configured total-position cap that did nothing, and lot size still computed
as 100 shares.

---

## Testing

```bash
./build/backtest_tests                                        # 92 cases
./build/backtest_tests --gtest_filter='GoldenIndicator*'      # indicator golden fixture
```

### The indicator golden fixture

`tests/data/indicator_golden.json` pins `macd` / `rsi` / `kdj` / `bollinger` / `sma` /
`stddev` on **every bar** of four series — 180 real daily bars plus three constructed series
that force specific branches (flat bars hit `kdj`'s exact `hhv == llv` comparison; monotonic
series hit `rsi`'s `avg_loss == 0 → 100.0` path and its mirror).

Each value is stored as the **bit pattern** of the double, not a decimal, and compared as an
integer. `EXPECT_DOUBLE_EQ` is deliberately not used: it allows 4 ULPs of slack, which is
exactly the error class this fixture exists to catch. Measured — rewriting MACD's EMA
recurrence into the algebraically identical `p + α(x − p)` form produces 585 differing
fields, and **51 of them (8.7%) are within 4 ULPs**, i.e. `EXPECT_DOUBLE_EQ` would have passed
them silently.

The fixture was captured from the current naive implementations *before* any rewrite, which is
the only ordering under which it can prove anything. The generator is a separate target
(`indicator_golden_gen`) rather than a `--regen` flag on the test, so that the test has no
ability to overwrite its own expectations.
