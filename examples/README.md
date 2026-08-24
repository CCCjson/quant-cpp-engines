# examples

`python_client.py` — 两个引擎的演示客户端，**只用 Python 标准库**（`urllib` + `json`），不需要 `pip install` 任何东西。

## 运行

```bash
# 先构建（如果还没构建过）
../build.sh

# 起两个服务器
../backtest_engine/build/backtest_server 8002 &
../orderbook_simulator/build/orderbook_server 8001 &

python3 python_client.py
```

只起了其中一个服务器也能跑——另一个会被跳过。

## 它演示了什么

**回测引擎**

- `GET /api/strategies` 列出全部内置策略及其参数 schema
- 用一段**确定性合成行情**（不依赖任何数据源，可复现）跑 `MA_CROSS`
- 同一份数据开/关风控（5% 止损 + 8% 追踪止损 + 必须留 20% 现金）对照
- `run_signals` 外部信号回放
- `run_portfolio` 两个标的**共享同一份现金**的组合回测
- 两个**故意打错**的请求，验证引擎拒绝静默失败：日期窗口空 → `400`，组合腿 symbol 重复 → `400`

**订单簿模拟器**

- 创建会话并播种 200 个初始订单，打印盘口
- LIMIT 买单穿过 `best_ask` 吃掉对手盘
- MARKET 大额卖单**吃穿多个价位**，打印实际滑价——这正是用收盘价成交的回测看不见的市场冲击
- FOK 巨单 → 整单拒绝，`filled_quantity=0`
- IOC 单 → 成交一部分，剩余**立刻取消**而不是挂单
- 挂单 + 撤单
- 盘口统计（价差 bps、深度、买卖失衡、VWAP）
- 撮合前后的盘口对照——大额卖单把价差从 3 个 tick 拉宽到 10 个

## 输出节选

```
▸ POST orders — MARKET 大额卖单（无条件吃买盘，会吃穿多个价位）
    市价卖 8000 股 → 成交 8000 股，分 14 笔
    吃穿 5 个价位：最好 99.99 → 最差 99.95，成交均价 99.9749
    相对最优价滑价 0.0151% ← 这就是市场冲击，用收盘价成交的回测看不见它

▸ POST orders — FOK 巨单（对手盘不够 → 整单拒绝，一股都不成交）
    is_rejected=True  filled_quantity=0  is_resting=False

▸ 拒绝静默失败：日期窗口内一根 bar 都没有
    HTTP 400 → No bars fall inside the requested date range.
```

> 合成行情用的是自己实现的线性同余随机数，不依赖 `random` 模块的实现细节——换机器、换 Python 版本，输出完全一致。
