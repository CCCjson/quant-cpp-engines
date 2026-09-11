# backtest_engine

*[English version](README.md)*

事件驱动的股票/加密货币回测引擎，C++17 编写，自带 REST API 服务器（默认 `:8002`）。

**约 7.4k 行 C++ · 10 个内置策略 · 82 个 GoogleTest 用例全绿 · `-Wall -Wextra -Werror` 零警告**

---

## 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

./build/backtest_tests          # 跑单元测试
./build/backtest_server         # 启动服务器（默认 8002）
./build/backtest_server 9000    # 指定端口
```

## 架构

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
             │  逐 bar 事件循环：                  │
             │    settle_t1() → mark_to_market   │
             │    → RiskManager 检查止损          │
             │    → strategy.on_bar(ctx)         │
             │    → 订单排队到 N+1 开盘成交         │
             └───┬──────────┬──────────┬────────┘
                 │          │          │
       ┌─────────▼──┐  ┌────▼─────┐  ┌─▼──────────┐
       │ IStrategy  │  │ Portfolio│  │ Metrics    │
       │ 纯虚基类     │  │ 仓位/现金 │  │ 19 项指标   │
       │ 10 个实现   │  │ T+1 冻结  │  │ 夏普/回撤等 │
       └────────────┘  └──────────┘  └────────────┘
```

| 文件 | 职责 |
|---|---|
| `include/backtest/types.h` | `Bar` / `Order` / `Fill` / `Position` / `EquitySnapshot` / `CommissionConfig` / `MarketRules` |
| `include/backtest/strategy_base.h` | `IStrategy` 纯虚基类——`name()` / `description()` / `param_schema()` / `on_bar()` |
| `include/backtest/strategy_context.h` | 传给策略的只读上下文（历史 bar、当前持仓、现金） |
| `src/engine.cpp` | 逐 bar 事件循环、订单撮合、T+1 结算 |
| `src/portfolio.cpp` | 仓位管理、现金追踪、可卖数量冻结/解冻 |
| `src/risk_manager.cpp` | 固定止损、追踪止损、单标的仓位上限、总仓位上限 |
| `src/metrics.cpp` | 收益率、波动率、最大回撤、夏普、索提诺、胜率、盈亏比 |
| `src/data_loader.cpp` | JSON / CSV 载入，以及测试用的模拟数据生成 |
| `src/server.cpp` | REST API 路由 |

## 内置策略

| name | 说明 | 主要参数 |
|---|---|---|
| `MA_CROSS` | 均线交叉：短期均线上穿长期均线买入，下穿卖出 | `fast_period=5` `slow_period=20` |
| `MOMENTUM` | 动量：过去 N 天涨幅超阈值买入，跌幅超阈值卖出 | `lookback=20` `buy_threshold=0.05` `sell_threshold=-0.03` |
| `MACD` | DIF 上穿 DEA 买入，下穿卖出 | `fast_period=12` `slow_period=26` `signal_period=9` |
| `RSI` | RSI 低于超卖区回升买入，高于超买区回落卖出 | `period=14` `oversold=30` `overbought=70` |
| `KDJ` | K 线低位上穿 D 线买入，高位下穿卖出 | `n=9` `m1=3` `m2=3` `oversold=20` `overbought=80` |
| `BOLLINGER` | 价格触及下轨反弹买入，触及上轨回落卖出 | `period=20` `num_std=2.0` |
| `COMBO` | 多个子策略加权融合信号，超过阈值触发 | `threshold=0.5` `sub_strategies=[{name,weight,params}]` |
| `PAIRS` | 配对交易（两条腿轮动，long-only），按价差 z-score 切换 | `symbol2` `lookback=60` `entry_z=2.0` `exit_z=0.5` |
| `SIGNAL` | 外部信号回放：按 `[{date, action, price?, weight?}]` 序列成交 | —（走 `/run_signals`） |
| `PORTFOLIO_SIGNAL` | 组合信号回放：每标的一条信号序列，**共享一份资金** | —（走 `/run_portfolio`） |

所有策略共用 `position_pct`（仓位比例，默认 `0.95`）。

> `GET /api/strategies` 返回的是**前 8 个**——`SIGNAL` 和 `PORTFOLIO_SIGNAL` 不由 `strategy` 字段选择，而是由 `/run_signals`、`/run_portfolio` 两个专用端点隐式装配，所以不进策略目录。

## 市场预设

`market` 字段决定手续费、印花税、滑点和最小交易单位。四套预设写在 `types.h` 的工厂方法里：

| market | 佣金率 | 印花税 | 最低佣金 | 默认滑点 | 一手 |
|---|---|---|---|---|---|
| `a_share` (默认) | 万 2.5 | 千 1（仅卖出） | 5.0 | 0.1% | 100 |
| `us` | 万 1 | 无 | 1.0 | 0.05% | 1 |
| `hk` | 万 5 | 千 1（双边） | 5.0 | 0.1% | 1 |
| `crypto` | 千 1（taker） | 无 | 无 | 0.05% | 1 |

`slippage_pct` 可在请求里显式覆盖；`risk_config` 里的 `max_position_pct`（单标的上限）与 `max_total_position_pct`（总仓位上限）是**两条独立约束，引擎取更严的那个**——取 `max` 会让「必须留 20% 现金」这条保护静默失效。

## REST API

### `GET /api/strategies`

返回策略清单及其参数 schema。

```bash
curl http://127.0.0.1:8002/api/strategies
```

### `POST /api/backtest/run` — 单标的策略回测

```jsonc
{
  "symbol": "AAPL",
  "strategy": "MA_CROSS",
  "params": { "fast_period": 5, "slow_period": 20 },
  "bars": [{ "date": "2025-01-02", "open": 100, "high": 102, "low": 99, "close": 101, "volume": 1000000 }],
  "initial_capital": 100000,
  "market": "us",              // us | hk | a_share | crypto
  "slippage_pct": 0.001,       // 可选，缺省用市场默认
  "risk_config": {             // 可选
    "enabled": true,
    "stop_loss_pct": 0.05,
    "trailing_stop": true,
    "trailing_stop_pct": 0.08,
    "max_position_pct": 1.0,
    "max_total_position_pct": 0.8
  },
  "start_date": "2025-01-01",  // 可选
  "end_date":   "2025-12-31"   // 可选
}
```

> `bars` 缺省时会生成模拟数据，方便快速试跑。

### `POST /api/backtest/run_signals` — 外部信号回放

把策略换成一条信号序列，撮合/费用/滑点/T+1 全部复用同一套引擎。`bars` **必填**。

```jsonc
{
  "symbol": "AAPL",
  "signals": [{ "date": "2025-01-15", "action": "buy", "weight": 0.9, "price": 12.3 }],
  "bars": [ /* ... */ ],
  "initial_capital": 100000,
  "market": "us"
}
```

`action` 大小写不敏感；`weight` 缺省 `0.95`；`price` 缺省即市价单。

### `POST /api/backtest/run_portfolio` — 组合回测

N 个标的**共享同一份现金**。某条腿只给 `bars` 不给 `signals` 是合法的——那个标的只当行情背景（比如配对交易的另一条腿），不产生订单。

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

### 响应格式（三个端点一致）

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

  // ── 不静默：下面三个字段让调用方能核对回测到底跑了什么 ──
  "dropped_last_bar_orders": 0,                       // 最后一根 bar 因无「次日开盘」被丢弃的挂单数
  "bar_coverage": { "AAPL": 251 },                    // 每标的实际有多少天有 bar（缺 bar ≠ 数据是 0）
  "cash_contention": { "days": 3, "trimmed_notional": 8200.0 }  // 资金竞争导致的等比缩减
}
```

`trades[].reason` 取值：`signal` / `stop_loss` / `trailing_stop`。

> ⚠️ `metrics.total_trades` 数的是**完成的往返**（一买一卖算一次），不是 `trades` 数组的长度——最后一笔还没平的持仓不计入。
> ⚠️ `profit_factor` 在**零亏损交易**时被置为 `0.0`（除零保护），不是真的等于 0；判断时要先看 `losing_trades`。

## 几个刻意的设计

**信号在次日开盘成交。** 第 N 根 bar 上产生的信号，一律用第 N+1 根 bar 的开盘价成交。用当天收盘价成交当天的信号是最常见的前视偏差来源，这里从引擎层面堵死。

**T+1 是真建模的，不是注释。** `Position` 同时有 `quantity` 和 `available` 两个字段，当日买入的份额进 `quantity` 但不进 `available`，次日开盘 `settle_t1()` 才解冻。市场规则（`MarketRules.lot_size`）也按市场查表——A 股一手 100 股，加密货币一手 1 个单位。

**日期窗口空了报 400 而不是跑全量。** 调用方给的 `[start_date, end_date]` 里一根 bar 都没有，是**输入**问题，抛 `EmptyDateRange` → HTTP 400。静默跑全量数据再回 200，等于对调用方撒谎。

**组合回测拒绝重复 symbol。** `load_data` 是后者覆盖前者，静默塌成一条腿会让「10 个标的的组合」悄悄变成 3 个，而收益率看上去一切正常。所以重复 symbol 直接 400。

**三个端点共用同一套配置解析。** `commission_of()` / `risk_from_json()` / `market_rules_of()` 只有一份。曾经某个端点自己抄了一份 inline 逻辑，结果是那个端点读不到 `max_total_position_pct`、也拿不到 `MarketRules`——配了总仓位上限却不生效，一手仍然按 100 股算。
