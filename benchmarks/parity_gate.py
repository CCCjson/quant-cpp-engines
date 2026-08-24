#!/usr/bin/env python3
"""
parity_gate.py — 先证明两个引擎算的是同一件事，再谈谁快

两个引擎如果经济结果对不上，跑分就是在比较两个不同的东西，毫无意义。
所以这道门禁是硬的：不过就不出跑分。

用法：
    ./backtest_engine/build/backtest_server 8002 &
    conda run -n quant python benchmarks/parity_gate.py

数据：benchmarks/data/cpp_bars.json —— 180 根真实日线（2024-01-02 → 2024-09-27），
      取自原项目的 parity 基线 fixture。

⚠️ 两边的**表示口径**不同，但那不是分歧，是单位和数据形状的差别：
     · Python 的收益率/回撤/胜率是**百分数**（44.85），C++ 是**小数**（0.4485）
     · Python 的 max_drawdown 是嵌套 dict，C++ 是平铺字段
   本文件把换算逐条写死并标注出来，不做任何"凑近"的处理。
"""

from __future__ import annotations

import json
import pathlib
import sys
import urllib.error
import urllib.request
from typing import Any, Callable

from loguru import logger

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))

# ⚠️ 必须在跑引擎之前关掉日志：Python 引擎每笔成交都 logger.info（engine.py:113），外加结束时的完整报告，
#    开着既拖慢又刷屏。这里关的是**驱动侧的 sink**，引擎源码一行没改。
logger.remove()

import benchlib  # noqa: E402
from python_reference import BacktestEngine  # noqa: E402
from python_reference.strategies import MACrossStrategy  # noqa: E402

CPP_URL = "http://127.0.0.1:8002"
TOLERANCE = 1e-6

# (显示名, 从 Python metrics 取值, 从 C++ metrics 取值, 换算说明)
#
# 这六项是「这笔钱到底赚了多少」的口径，必须逐项对上。
MUST_MATCH: list[tuple[str, Callable[[dict], float], Callable[[dict], float], str]] = [
    ("最终资产",   lambda p: p["final_value"],                    lambda c: c["final_value"],          "直接可比"),
    ("总收益率",   lambda p: p["total_return"] / 100,             lambda c: c["total_return"],         "Python 百分数 ÷100"),
    ("年化收益率", lambda p: p["annualized_return"] / 100,        lambda c: c["annualized_return"],    "Python 百分数 ÷100"),
    ("最大回撤",   lambda p: p["max_drawdown"]["max_drawdown_pct"] / 100,
                                                                  lambda c: c["max_drawdown"],         "Python 嵌套 dict + 百分数 ÷100"),
    ("回撤金额",   lambda p: p["max_drawdown"]["max_drawdown"],   lambda c: c["max_drawdown_amount"],  "Python 嵌套 dict"),
    ("总手续费",   lambda p: p["total_commission"],               lambda c: c["total_commission"],     "直接可比"),
    ("胜率",       lambda p: p["win_rate"] / 100,                 lambda c: c["win_rate"],             "Python 百分数 ÷100"),
]


def run_cpp(bars: list[dict], request: dict) -> dict:
    body = dict(request, bars=bars)
    req = urllib.request.Request(
        f"{CPP_URL}/api/backtest/run",
        data=json.dumps(body).encode(),
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read().decode())


def run_python(bars: list[dict], request: dict) -> dict:
    df = benchlib.bars_to_df(bars)
    params = request["params"]
    df = benchlib.add_ma(df, (params["fast_period"], params["slow_period"]))

    engine = BacktestEngine(initial_capital=request["initial_capital"])
    strategy = MACrossStrategy(
        fast_period=params["fast_period"],
        slow_period=params["slow_period"],
        position_size=params["position_pct"],
    )
    return engine.run(request["symbol"], df, strategy)


def main() -> int:
    bars = json.loads((HERE / "data" / "cpp_bars.json").read_text())
    golden = json.loads((HERE / "data" / "cpp_parity_golden.json").read_text())
    request = golden["request"]

    print("=" * 78)
    print("  Parity 门禁 — Python 参照引擎 vs C++ 引擎")
    print("=" * 78)
    print(f"  数据    {len(bars)} 根真实日线  {bars[0]['date']} → {bars[-1]['date']}")
    print(f"  策略    {request['strategy']}  {request['params']}")
    print(f"  本金    {request['initial_capital']:,.0f}   市场 {request['market']}")
    print()

    try:
        cpp_full = run_cpp(bars, request)
    except (urllib.error.URLError, OSError) as exc:
        print(f"❌ C++ 服务未响应（{CPP_URL}）：{exc}")
        print("   先启动：./backtest_engine/build/backtest_server 8002 &")
        return 2

    cpp = cpp_full["metrics"]
    py_full = run_python(bars, request)
    py = py_full["metrics"]

    print(f"  {'指标':<12}{'Python':>20}{'C++':>20}{'差值':>12}   换算")
    print("  " + "-" * 92)
    failures: list[str] = []
    rows: list[dict[str, Any]] = []

    for label, get_py, get_cpp, note in MUST_MATCH:
        a, b = float(get_py(py)), float(get_cpp(cpp))
        diff = abs(a - b)
        ok = diff <= TOLERANCE
        print(f"  {'✓' if ok else '✗'} {label:<10}{a:>20.8f}{b:>20.8f}{diff:>12.2e}   {note}")
        rows.append({"metric": label, "python": a, "cpp": b, "diff": diff, "ok": ok})
        if not ok:
            failures.append(f"{label}: Python={a} C++={b} 差 {diff:.3e}")

    # ── 已知差异：登记，不掩盖 ──
    print()
    print("  已知口径差异（不参与门禁，但必须看得见）：")
    print(f"    成交笔数      Python={py['num_trades']} (fills)  ·  "
          f"C++={cpp['total_trades']} (round-trips)  —— 同一批交易，计数口径不同")
    d_sharpe = abs(float(py["sharpe_ratio"]) - float(cpp["sharpe_ratio"]))
    d_vol = abs(float(py["volatility"]) / 100 - float(cpp["volatility"]))
    print(f"    年化波动率    Python={float(py['volatility']) / 100:.10f}  ·  "
          f"C++={float(cpp['volatility']):.10f}  —— 差 {d_vol:.2e}")
    print(f"                  根因：pandas .std() 默认 ddof=1（样本标准差），"
          f"C++ 用 ddof=0（总体）。比值恰为 sqrt(n/(n-1))。")
    print(f"    夏普比率      Python={float(py['sharpe_ratio']):.6f}  ·  "
          f"C++={float(cpp['sharpe_ratio']):.6f}  —— 差 {d_sharpe:.4f}")
    print(f"                  根因：两种都是标准夏普算法，年化路径不同 ——")
    print(f"                    C++    = 日均算术超额收益 / 超额收益std × √252")
    print(f"                    Python = (几何年化收益 − rf) / 年化波动率")
    print(f"                  ⚠️ 原项目 2026-07-15 的退役记录写「夏普差 0.018」，"
          f"本 fixture 上实测差 {d_sharpe:.4f}。")
    print(f"                     两种年化路径在收益率序列偏度大时差距会放大，"
          f"当年应是用了另一组数据。")
    print(f"    零亏损哨兵    Python profit_factor={py['profit_factor']}  ·  "
          f"C++={cpp['profit_factor']}  —— 同一件事的两种表达（inf vs 除零保护置 0）")
    print(f"    回撤区间      Python={str(py['max_drawdown']['start_date'])[:10]}→"
          f"{str(py['max_drawdown']['end_date'])[:10]}  ·  "
          f"C++={cpp['max_dd_start_date']}→{cpp['max_dd_end_date']}")

    result = {
        "input": {"bars": len(bars), "request": request},
        "comparisons": rows,
        "known_differences": {
            "num_trades": {"python_fills": py["num_trades"], "cpp_round_trips": cpp["total_trades"]},
            "sharpe_ratio": {
                "python": py["sharpe_ratio"], "cpp": cpp["sharpe_ratio"], "diff": d_sharpe,
                "cause": "两种标准夏普算法：C++ 日均算术超额×sqrt(252)；Python (几何年化-rf)/年化波动率",
                "historical_record_2026_07_15": 0.018,
            },
            "volatility": {
                "python": float(py["volatility"]) / 100, "cpp": cpp["volatility"], "diff": d_vol,
                "cause": "pandas .std() ddof=1（样本） vs C++ ddof=0（总体），比值 sqrt(n/(n-1))",
            },
            "profit_factor_zero_loss_sentinel": {"python": str(py["profit_factor"]), "cpp": cpp["profit_factor"]},
        },
        "passed": not failures,
    }
    (HERE / "results").mkdir(exist_ok=True)
    benchlib.write_json(str(HERE / "results" / "parity.json"), result)

    print()
    if failures:
        print("  ❌ Parity 门禁未通过：")
        for f in failures:
            print(f"       · {f}")
        print("\n     两个引擎算的不是同一件事，跑分没有意义。先查口径。")
        return 1

    print("  ✅ Parity 门禁通过 —— 经济结果一致，可以跑性能对比。")
    print("     结果已写入 results/parity.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
