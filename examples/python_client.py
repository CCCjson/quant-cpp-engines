#!/usr/bin/env python3
"""
python_client.py — 两个 C++ 引擎的演示客户端

只用 Python 标准库，不需要 pip install 任何东西。

前置：先把两个服务器跑起来
    ./backtest_engine/build/backtest_server 8002 &
    ./orderbook_simulator/build/orderbook_server 8001 &

然后：
    python3 examples/python_client.py
"""

from __future__ import annotations

import json
import math
import urllib.error
import urllib.request
from typing import Any

BACKTEST_URL = "http://127.0.0.1:8002"
ORDERBOOK_URL = "http://127.0.0.1:8001"

TIMEOUT = 30.0


# ────────────────────────────────────────────────────────────
# 极简 HTTP 封装
# ────────────────────────────────────────────────────────────

def request(method: str, url: str, payload: dict | None = None) -> Any:
    """发一个 JSON 请求，返回解析后的响应体。

    HTTP 4xx/5xx 不当成崩溃——两个引擎都会在请求体有问题时返回结构化的
    {"error": "..."}，那正是需要打印出来看的东西。
    """
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            return json.loads(resp.read().decode() or "null")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode()
        try:
            return {"_http_status": exc.code, **json.loads(body)}
        except json.JSONDecodeError:
            return {"_http_status": exc.code, "error": body}


def get(url: str) -> Any:
    return request("GET", url)


def post(url: str, payload: dict) -> Any:
    return request("POST", url, payload)


def alive(url: str, probe: str) -> bool:
    try:
        get(url + probe)
        return True
    except (urllib.error.URLError, OSError):
        return False


def title(text: str) -> None:
    print()
    print("=" * 64)
    print(f"  {text}")
    print("=" * 64)


# ────────────────────────────────────────────────────────────
# 造一段确定性的合成行情（不依赖任何数据源）
# ────────────────────────────────────────────────────────────

def synth_bars(n: int = 250, start: float = 100.0, seed: int = 7) -> list[dict]:
    """生成 n 根日线。

    刻意用一个带趋势 + 正弦周期的确定性序列，而不是随机游走——
    这样均线交叉策略必然会产生若干次金叉死叉，演示输出才稳定可复现。
    """
    bars: list[dict] = []
    price = start
    rng = seed
    for i in range(n):
        # 线性同余随机数：把「可复现」这件事握在自己手里，不依赖 random 的实现
        rng = (rng * 1103515245 + 12345) % (2**31)
        noise = (rng / 2**31 - 0.5) * 0.012         # ±0.6% 噪声
        cycle = math.sin(i / 9.0) * 0.004           # 周期性波动，制造均线穿越
        drift = 0.0002                              # 轻微上行漂移
        price *= 1.0 + drift + cycle + noise

        close = round(price, 2)
        high = round(close * (1 + abs(noise) + 0.003), 2)
        low = round(close * (1 - abs(noise) - 0.003), 2)
        open_ = round((high + low) / 2, 2)
        # 日期从 2025-01-01 起按自然日递增（引擎只把 date 当有序标签，不做日历运算）
        day = 1 + i
        month = 1 + (day - 1) // 28
        bars.append({
            "date": f"2025-{month:02d}-{(day - 1) % 28 + 1:02d}",
            "open": open_, "high": high, "low": low, "close": close,
            "volume": 1_000_000 + (rng % 500_000),
        })
    return bars


def signals_from_bars(bars: list[dict], every: int = 40) -> list[dict]:
    """每隔 every 根 bar 交替产生一次买/卖信号，用来演示 run_signals。"""
    out = []
    for i in range(0, len(bars), every):
        out.append({
            "date": bars[i]["date"],
            "action": "buy" if (i // every) % 2 == 0 else "sell",
            "weight": 0.9,
        })
    return out


def show_metrics(result: dict) -> None:
    if "error" in result:
        print(f"  ⚠️  {result.get('_http_status', '')} {result['error']}")
        return
    m = result["metrics"]
    print(f"  策略           {result['strategy_name']}")
    print(f"  标的           {', '.join(result['symbols'])}")
    print(f"  总收益率        {m['total_return']:+.2%}")
    print(f"  年化收益率      {m['annualized_return']:+.2%}")
    print(f"  最终资产        {m['final_value']:,.2f}")
    print(f"  年化波动率      {m['volatility']:.2%}")
    print(f"  最大回撤        {m['max_drawdown']:.2%}"
          f"  ({m['max_dd_start_date']} → {m['max_dd_end_date']})")
    print(f"  夏普 / 索提诺   {m['sharpe_ratio']:.2f} / {m['sortino_ratio']:.2f}")
    print(f"  交易次数        {m['total_trades']}  "
          f"(赢 {m['winning_trades']} / 输 {m['losing_trades']}, 胜率 {m['win_rate']:.1%})")
    # profit_factor 在「零亏损交易」时被引擎置为 0.0（除零保护），不是真的等于 0
    pf = (f"{m['profit_factor']:.2f}" if m["losing_trades"] > 0 else "— (无亏损交易)")
    print(f"  盈亏比          {pf}")
    print(f"  手续费 / 滑点   {m['total_commission']:,.2f} / {m['total_slippage']:,.2f}")
    # ── 这三个字段是「拒绝静默失败」的体现，演示里一定要打出来 ──
    print(f"  数据覆盖        {result['bar_coverage']}")
    print(f"  末根被弃挂单    {result['dropped_last_bar_orders']}")
    cc = result["cash_contention"]
    print(f"  资金竞争        {cc['days']} 天, 缩减名义额 {cc['trimmed_notional']:,.2f}")


# ────────────────────────────────────────────────────────────
# 演示 1：回测引擎
# ────────────────────────────────────────────────────────────

def demo_backtest() -> None:
    title("回测引擎 :8002")

    strategies = get(f"{BACKTEST_URL}/api/strategies")
    print(f"\n▸ 可用策略（{len(strategies)} 个）")
    for s in strategies:
        print(f"    {s['name']:<18} {s['description']}")

    bars = synth_bars(250)
    print(f"\n▸ 合成行情：{len(bars)} 根日线，"
          f"{bars[0]['date']} → {bars[-1]['date']}，"
          f"{bars[0]['close']} → {bars[-1]['close']}")

    print("\n▸ POST /api/backtest/run — MA_CROSS 策略")
    show_metrics(post(f"{BACKTEST_URL}/api/backtest/run", {
        "symbol": "DEMO",
        "strategy": "MA_CROSS",
        "params": {"fast_period": 5, "slow_period": 20, "position_pct": 0.95},
        "bars": bars,
        "initial_capital": 100_000,
        "market": "us",
    }))

    print("\n▸ POST /api/backtest/run — 同样的数据，开启风控（5% 止损 + 8% 追踪止损）")
    show_metrics(post(f"{BACKTEST_URL}/api/backtest/run", {
        "symbol": "DEMO",
        "strategy": "MA_CROSS",
        "params": {"fast_period": 5, "slow_period": 20, "position_pct": 0.95},
        "bars": bars,
        "initial_capital": 100_000,
        "market": "us",
        "risk_config": {
            "enabled": True,
            "stop_loss_pct": 0.05,
            "trailing_stop": True,
            "trailing_stop_pct": 0.08,
            "max_total_position_pct": 0.8,   # 必须留 20% 现金
        },
    }))

    print("\n▸ POST /api/backtest/run_signals — 外部信号回放")
    show_metrics(post(f"{BACKTEST_URL}/api/backtest/run_signals", {
        "symbol": "DEMO",
        "signals": signals_from_bars(bars),
        "bars": bars,
        "initial_capital": 100_000,
        "market": "us",
    }))

    print("\n▸ POST /api/backtest/run_portfolio — 两个标的共享同一份现金")
    bars_b = synth_bars(250, start=60.0, seed=99)
    show_metrics(post(f"{BACKTEST_URL}/api/backtest/run_portfolio", {
        "legs": [
            {"symbol": "AAA", "bars": bars, "signals": signals_from_bars(bars, 40)},
            {"symbol": "BBB", "bars": bars_b, "signals": signals_from_bars(bars_b, 55)},
        ],
        "initial_capital": 100_000,
        "market": "crypto",
        "risk_config": {"enabled": True, "max_total_position_pct": 0.8},
    }))

    # ── 拒绝静默失败：下面两个请求应该拿到 400，而不是一个看似正常的结果 ──
    print("\n▸ 拒绝静默失败：日期窗口内一根 bar 都没有")
    bad = post(f"{BACKTEST_URL}/api/backtest/run", {
        "symbol": "DEMO", "strategy": "MA_CROSS", "bars": bars,
        "start_date": "2099-01-01", "end_date": "2099-12-31",
    })
    print(f"    HTTP {bad.get('_http_status')} → {bad.get('error')}")

    print("\n▸ 拒绝静默失败：组合回测里两条腿 symbol 重复")
    bad = post(f"{BACKTEST_URL}/api/backtest/run_portfolio", {
        "legs": [{"symbol": "AAA", "bars": bars}, {"symbol": "AAA", "bars": bars_b}],
        "initial_capital": 100_000,
    })
    print(f"    HTTP {bad.get('_http_status')} → {bad.get('error')}")


# ────────────────────────────────────────────────────────────
# 演示 2：订单簿模拟器
# ────────────────────────────────────────────────────────────

def show_depth(depth: dict, levels: int = 5) -> None:
    asks = depth["asks"][:levels][::-1]
    bids = depth["bids"][:levels]
    for lv in asks:
        print(f"          {lv['price']:>8.2f}  │ {lv['quantity']:>6}  ({lv['order_count']} 单)")
    print(f"    ──────────────────┼──── spread {depth['spread']:.2f}"
          f"   mid {depth['mid_price']:.2f} ────")
    for lv in bids:
        print(f"          {lv['price']:>8.2f}  │ {lv['quantity']:>6}  ({lv['order_count']} 单)")


def demo_orderbook() -> None:
    title("订单簿模拟器 :8001")

    session = post(f"{ORDERBOOK_URL}/api/sessions", {
        "symbol": "DEMO", "mid_price": 100.0, "seed_count": 200,
        "tick_size": 0.01, "spread_ticks": 2,
    })
    sid = session["session_id"]
    print(f"\n▸ 创建会话 {sid}（播种 {session['seed_count']} 单）")

    print("\n▸ GET depth — 初始盘口（卖盘在上，买盘在下）")
    show_depth(get(f"{ORDERBOOK_URL}/api/sessions/{sid}/depth?levels=5"))

    print("\n▸ POST orders — LIMIT 买单，价格穿过 best_ask（会吃掉对手盘）")
    depth = get(f"{ORDERBOOK_URL}/api/sessions/{sid}/depth?levels=5")
    aggressive_price = round(depth["asks"][2]["price"], 2)
    res = post(f"{ORDERBOOK_URL}/api/sessions/{sid}/orders", {
        "side": "BUY", "order_type": "LIMIT",
        "price": aggressive_price, "quantity": 400,
    })
    print(f"    限价 {aggressive_price} 买 400 股")
    print(f"    成交 {res['filled_quantity']} 股，分 {len(res['fills'])} 笔；"
          f"剩余 {res['remaining_quantity']} 股"
          f"{'挂在簿上' if res['is_resting'] else '未挂单'}")

    print("\n▸ POST orders — MARKET 大额卖单（无条件吃买盘，会吃穿多个价位）")
    res = post(f"{ORDERBOOK_URL}/api/sessions/{sid}/orders", {
        "side": "SELL", "order_type": "MARKET", "quantity": 8000,
    })
    print(f"    市价卖 8000 股 → 成交 {res['filled_quantity']} 股，"
          f"分 {len(res['fills'])} 笔")
    if res["fills"]:
        prices = [f["price"] for f in res["fills"]]
        vwap = sum(f["price"] * f["quantity"] for f in res["fills"]) / res["filled_quantity"]
        slip = (max(prices) - vwap) / max(prices)
        print(f"    吃穿 {len(set(prices))} 个价位：最好 {max(prices):.2f} → 最差 {min(prices):.2f}，"
              f"成交均价 {vwap:.4f}")
        print(f"    相对最优价滑价 {slip:.4%} ← 这就是市场冲击，"
              f"用收盘价成交的回测看不见它")

    print("\n▸ POST orders — FOK 巨单（对手盘不够 → 整单拒绝，一股都不成交）")
    res = post(f"{ORDERBOOK_URL}/api/sessions/{sid}/orders", {
        "side": "BUY", "order_type": "FOK", "price": 200.0, "quantity": 10_000_000,
    })
    print(f"    is_rejected={res['is_rejected']}  "
          f"filled_quantity={res['filled_quantity']}  "
          f"is_resting={res['is_resting']}")

    print("\n▸ POST orders — IOC 单（能成多少成多少，剩余立刻取消，绝不挂单）")
    res = post(f"{ORDERBOOK_URL}/api/sessions/{sid}/orders", {
        "side": "BUY", "order_type": "IOC", "price": aggressive_price, "quantity": 100_000,
    })
    print(f"    限价 {aggressive_price} 买 100000 股（远超该价位以内的可用量）")
    print(f"    成交 {res['filled_quantity']}，"
          f"剩余 {res['remaining_quantity']} 立刻取消，"
          f"is_resting={res['is_resting']}  ← 换成 LIMIT 这里就会挂 {res['remaining_quantity']} 股在簿上")

    print("\n▸ POST orders + DELETE — 挂一个远离盘口的单再撤掉")
    resting = post(f"{ORDERBOOK_URL}/api/sessions/{sid}/orders", {
        "side": "BUY", "order_type": "LIMIT", "price": 50.0, "quantity": 100,
    })
    oid = resting["order_id"]
    print(f"    挂单 {oid} → is_resting={resting['is_resting']}")
    cancelled = request("DELETE", f"{ORDERBOOK_URL}/api/sessions/{sid}/orders/{oid}")
    print(f"    撤单 → success={cancelled['success']}")

    print("\n▸ GET stats — 盘口统计")
    st = get(f"{ORDERBOOK_URL}/api/sessions/{sid}/stats")
    print(f"    价差          {st['spread']:.2f}  ({st['spread_bps']:.1f} bps)")
    print(f"    中间价        {st['mid_price']:.2f}")
    print(f"    买/卖盘深度   {st['bid_depth']} / {st['ask_depth']}")
    print(f"    买卖失衡      {st['imbalance']:+.4f}   (>0 买盘更厚)")
    print(f"    VWAP          {st['vwap']:.4f}")
    print(f"    累计成交      {st['total_fills']} 笔 / {st['total_volume']} 股")

    print("\n▸ GET depth — 撮合之后的盘口")
    show_depth(get(f"{ORDERBOOK_URL}/api/sessions/{sid}/depth?levels=5"))


# ────────────────────────────────────────────────────────────

def main() -> None:
    bt_up = alive(BACKTEST_URL, "/api/strategies")
    ob_up = alive(ORDERBOOK_URL, "/health")

    if not bt_up and not ob_up:
        print("两个服务器都没起来。先执行：")
        print("    ./backtest_engine/build/backtest_server 8002 &")
        print("    ./orderbook_simulator/build/orderbook_server 8001 &")
        raise SystemExit(1)

    if bt_up:
        demo_backtest()
    else:
        print(f"⚠️  跳过回测引擎演示（{BACKTEST_URL} 未响应）")

    if ob_up:
        demo_orderbook()
    else:
        print(f"⚠️  跳过订单簿演示（{ORDERBOOK_URL} 未响应）")

    print()


if __name__ == "__main__":
    main()
