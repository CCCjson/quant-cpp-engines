# python_reference/ 的出处与改动清单

## 它是什么

`python_reference/` 里的代码**不是为了这次 benchmark 写的对照组**，而是一个真实跑过生产的
Python 回测引擎。它来自本仓库两个 C++ 引擎的母项目，在 C++ 版本成熟后被整体退役。

这件事很重要 —— 性能对比最常见的作弊方式，就是拿一个随手写的、没优化过的实现去比
一个精心打磨的实现。这里不存在那个问题：**这份 Python 代码当年是生产路径本身**。

## 取回方式

母项目里的退役 commit：

```
commit 288ad13  (2026-07-15)
    13 域5-4①：Python backtest_engine 退役硬删（后端）
```

用它的父提交把文件取出来（`288ad13^` = 删除发生前的最后状态）：

```bash
git show 288ad13^:backend/backtest_engine/engine.py               > python_reference/engine.py
git show 288ad13^:backend/backtest_engine/portfolio/portfolio.py  > python_reference/portfolio/portfolio.py
# …以此类推
```

退役时的完整目录是 17 个文件、1,775 行。

## 逐条改动清单

搬进本仓库时的改动**仅此三项**，全部列在下面。引擎的计算逻辑一行未动。

### 1. 移除 `backtest_executor.py`（−286 行）

它不是引擎，是母项目里的**任务编排层** —— 从数据库取行情、建任务记录、写回结果。
依赖 `data_engine` / `analysis_engine` / `HistoryRepository`，全都是母项目的东西。

benchmark 直接调 `BacktestEngine.run()`，用不到它。

### 2. 移除 `strategies/signal_strategy.py`（−74 行）+ 从 `strategies/__init__.py` 去掉它的导出

它 `from analysis_engine import AnalysisEngine`，是四个策略里**唯一**带母项目依赖的。
而且它对应的是 C++ 侧的 `SIGNAL` 策略（走 `/run_signals` 专用端点），
本来就不在对比范围内。

### 3. `portfolio/portfolio.py::infer_market` 内联（±0 行逻辑）

原版：

```python
def infer_market(symbol: Optional[str]) -> str:
    from common.market import infer_market_from_symbol
    return infer_market_from_symbol(symbol)
```

母项目把 symbol→市场的推断收敛成了单一真源 `common/market.py`。那个模块不在这里，
所以按它的原实现（`common/market.py:117-134`）逐行内联，语义完全一致：
`.SH/.SZ/.BJ` → A股，`.HK` → 港股，`.BN` → 加密货币，其余 → 美股，空值兜底 A股。

> ⚠️ 这个 import 藏在**函数体内**（延迟 import），不在文件顶部。
> 按模块级 import 扫一遍是发现不了它的 —— 是运行时炸出来的。

**改动后：14 个文件，1,413 行。**

## 依赖

只需要三个包，都是常见的：

```
pandas   （实测 2.1.4）
numpy    （实测 1.26.3）
loguru
```

不需要母项目的任何东西。

## 关于日志

引擎在 `engine.py:113` 处每笔成交打一条 `logger.info`，另有开头/结尾若干条，
以及结束时 `ReportGenerator.log_report()` 的完整报告。

benchmark 跑之前会 `logger.remove()` 把 sink 摘掉 —— 否则测的是终端 I/O 不是引擎。
**这是在驱动侧做的，引擎源码没改。** 开日志到底慢多少单独测了，见
`benchmarks/README.md` 的「附加测量」。

## 授权

与本仓库其余部分同属一个作者，同为 MIT。
