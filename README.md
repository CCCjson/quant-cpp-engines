# quant-cpp-engines

> 两个用 C++17 从零手写的量化交易底层引擎：**事件驱动回测引擎** 与 **限价订单簿撮合模拟器**。
> 各自自带 REST API 服务器，零外部依赖安装（CMake FetchContent 自动拉取），开箱即用。

```
┌──────────────────────────────────────────────────────────────┐
│                    调用方（任意语言 / HTTP）                    │
│              Python · TypeScript · curl · Postman            │
└───────────────┬──────────────────────────┬───────────────────┘
                │ JSON over HTTP           │ JSON over HTTP
                ▼                          ▼
┌───────────────────────────┐  ┌───────────────────────────────┐
│  backtest_engine  :8002   │  │  orderbook_simulator  :8001   │
│                           │  │                               │
│  ┌─────────────────────┐  │  │  ┌─────────────────────────┐  │
│  │  BacktestEngine     │  │  │  │  MatchingEngine         │  │
│  │  逐 bar 事件循环      │  │  │  │  价格优先 / 时间优先      │  │
│  └──────────┬──────────┘  │  │  └───────────┬─────────────┘  │
│             │             │  │              │                │
│  ┌──────────▼──────────┐  │  │  ┌───────────▼─────────────┐  │
│  │  IStrategy (10 个)  │  │  │  │  LimitOrderBook          │  │
│  │  策略模式 + 纯虚基类   │  │  │  │  双向价格树 (bids/asks)   │  │
│  └──────────┬──────────┘  │  │  └───────────┬─────────────┘  │
│             │             │  │              │                │
│  ┌──────────▼──────────┐  │  │  ┌───────────▼─────────────┐  │
│  │  Portfolio          │  │  │  │  PriceLevel (FIFO 队列)   │  │
│  │  RiskManager        │  │  │  │  MarketImpact / Stats    │  │
│  │  Metrics            │  │  │  │  Session / SessionMgr    │  │
│  └─────────────────────┘  │  │  └─────────────────────────┘  │
└───────────────────────────┘  └───────────────────────────────┘
        约 7.4k 行 C++                   约 3.2k 行 C++
        82 个 GoogleTest 用例             40 个 GoogleTest 用例
```

---

## 目录

| 目录 | 说明 |
|---|---|
| [`backtest_engine/`](backtest_engine/) | 事件驱动回测引擎，10 个策略实现（8 个进策略目录 + 2 个信号回放），组合回测，风控/费用/滑点建模 |
| [`orderbook_simulator/`](orderbook_simulator/) | 限价订单簿 + 撮合引擎，四种订单类型，市场冲击估算，多会话隔离 |
| [`examples/`](examples/) | 纯 stdlib 的 Python 演示客户端，一条命令跑通两个引擎 |
| [`benchmarks/`](benchmarks/) | **性能调查**：C++ vs 真实 Python 参照引擎，七个假设逐一验证 + 对照实验 |
| [`docs/`](docs/) | 原始设计文档 |

---

## 快速开始

需要 **CMake ≥ 3.16** 与支持 **C++17** 的编译器（clang / gcc / MSVC）。
第一次构建会自动从 GitHub 拉取三个 header-only 依赖，请保持联网。

```bash
# 一键构建两个引擎并跑全部单元测试
./build.sh

# 或者分别构建
cmake -S backtest_engine -B backtest_engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build backtest_engine/build -j8
./backtest_engine/build/backtest_tests

cmake -S orderbook_simulator -B orderbook_simulator/build -DCMAKE_BUILD_TYPE=Release
cmake --build orderbook_simulator/build -j8
./orderbook_simulator/build/orderbook_tests
```

启动两个服务器：

```bash
./backtest_engine/build/backtest_server 8002 &
./orderbook_simulator/build/orderbook_server 8001 &
```

跑演示（只用 Python 标准库，不装任何包）：

```bash
python3 examples/python_client.py
```

---

## 性能：结论不是「C++ 更快」

拿一个**真实跑过生产的 Python 回测引擎**（从母项目 git 历史里取回，见
[`benchmarks/ORIGIN.md`](benchmarks/ORIGIN.md)）做对照，在同一份数据上做了一次完整调查。

起点的假设是「C++ 是编译型所以更快」。**这个假设在第一轮数据里就被证伪了**：

| 策略（25,000 根 bar） | C++ vs Python |
|---|---|
| `MA_CROSS` | **242×** |
| `MACD` | **0.9×** ← C++ 输了 |

同一个语言、同一个引擎、同一份数据，换个策略结论就反过来。

顺着查下去，真正的原因是**算法复杂度阶数**：引擎里的 `macd()`/`rsi()`/`kdj()`
每根 bar 都从头重算整条序列（log-log 拟合斜率 k≈1.95，即 O(N²)），而 Python 侧的
指标是 numpy 一次性算好的 O(N)。

写了个逐位等价的增量版验证 —— 提速 **277×**，
MACD 立刻回到 `MA_CROSS` 的量级，对 Python 从输 0.9× 变成赢
248×。

> **算法阶数带来的差距，比语言选择带来的差距更大** —— 前者随 N 无限放大，后者是有上限的常数。

完整的七个假设验证、对照实验、方法论与已知局限：**[`benchmarks/README.md`](benchmarks/README.md)**

---

## 依赖

三个依赖全部由 CMake `FetchContent` 在构建时自动下载，**不需要手动 `apt install` / `brew install` 任何东西**：

| 依赖 | 用途 | 形态 |
|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) `v3.11.3` | JSON 序列化 | header-only |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | REST API 服务器 | header-only |
| [GoogleTest](https://github.com/google/googletest) `v1.14.0` | 单元测试 | 构建期 |

---

## 设计取向

这两个引擎不是玩具 demo，是从一套实盘量化系统里抽出来的生产组件。几处刻意的设计：

**1. 防未来函数（look-ahead bias）是硬约束**
回测引擎中，策略在第 N 根 bar 上产生的信号，一律在**第 N+1 根 bar 的开盘价**成交，绝不允许用当天收盘价成交当天的信号。A 股 T+1 规则也在 `Position.available` 上真实建模——当日买入的股份不可卖出，次日开盘才解冻。

**2. 拒绝静默失败**
输入有问题就报 400，不糊弄过去返回一个看似正常的 200：
- 日期窗口内一根 bar 都没有 → `400`，而不是静默跑全量数据；
- 组合回测里两条腿 symbol 重复 → `400`，而不是后者覆盖前者让「10 个标的的组合」悄悄塌成 3 个；
- 回测最后一根 bar 因为没有「次日开盘」而被丢弃的挂单数，作为 `dropped_last_bar_orders` 显式返回；
- 每个标的实际有多少天有数据，作为 `bar_coverage` 返回——**缺 bar ≠ 数据是 0**。

**3. 组合回测共享同一份现金**
N 个标的跑组合回测时共用一个资金池，而不是各发一份完整本金独立跑完再把收益率平均。没有资金竞争，带权重的组合策略回测出来的数字跟权重毫无关系。同一天多个买单抢同一份现金时按名义额等比缩减，缩减了多少通过 `cash_contention` 返回。

**4. 撮合遵循真实交易所语义**
价格优先、时间优先（同价位 FIFO）。MARKET / LIMIT / IOC / FOK 四种订单类型各自的语义都完整实现——FOK 是真的做「全有或全无」的预检，而不是先撮合再回滚。

---

## 许可

MIT
