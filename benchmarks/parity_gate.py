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

============================================================
门禁覆盖哪些策略，为什么是这三个
============================================================

很长一段时间里这道门禁只跑 MA_CROSS，而 MA_CROSS 只用 sma()。也就是说
「指标改增量递推」那一轮改掉的 macd/rsi/kdj，**一个都没进门禁** ——
号称最硬的那道闸门，守的不是被改动的代码。

现在按实测结果扩到三个：

    MA_CROSS   七项经济指标 0.00e+00   ✅ 门禁
    MACD       七项经济指标 0.00e+00   ✅ 门禁
    KDJ        七项经济指标 0.00e+00   ✅ 门禁
    RSI        终值差 5.58e+04         ❌ 不进门禁，作为**已登记的分歧**报告

⚠️ RSI 不进门禁，不是因为把闸门放宽了，而是因为两边算的**本来就是两个
   不同定义的 RSI**（详见下面 rsi_divergence 的输出）。把它塞进门禁只有两条路：
   放宽容差，或者改 Python 参照实现 —— 前者毁掉门禁，后者毁掉对照组。
   所以第三条路：查清、量化、写下来，闸门本身一寸不让。

⚠️ 还有一点别误读：门禁证明的是**经济结果**相等，不是**指标位模式**相等。
   MACD 两侧的指标值仍有 ≤6144 ULP 的分歧（相对 3.8e-14），只是离
   dif/dea 翻转还差 7.2e11 倍的余量，落不到成交上。

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
from python_reference.strategies import (  # noqa: E402
    KDJStrategy, MACDStrategy, MACrossStrategy, RSIStrategy,
)

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
    """按策略名分派到 Python 参照引擎。

    ⛔ 这里只**调用** python_reference/ 与 benchlib，一行都不改它们。
       它们是冻结的对照臂；改了对照物，对照就不成立了。
    """
    df = benchlib.bars_to_df(bars).copy()
    params = request["params"]
    name = request["strategy"]
    cap = request["initial_capital"]

    if name == "MA_CROSS":
        df = benchlib.add_ma(df, (params["fast_period"], params["slow_period"]))
        strategy = MACrossStrategy(fast_period=params["fast_period"],
                                   slow_period=params["slow_period"],
                                   position_size=params["position_pct"])
    elif name == "MACD":
        df = benchlib.add_macd(df, params["fast_period"], params["slow_period"],
                               params["signal_period"])
        strategy = MACDStrategy(position_size=params["position_pct"])
    elif name == "KDJ":
        df = benchlib.add_kdj(df, params["n"], params["m1"], params["m2"])
        strategy = KDJStrategy(oversold=params["oversold"],
                               overbought=params["overbought"],
                               position_size=params["position_pct"])
    elif name == "RSI":
        df = benchlib.add_rsi(df, params["period"])
        strategy = RSIStrategy(oversold=params["oversold"],
                               overbought=params["overbought"],
                               position_size=params["position_pct"])
    else:
        raise ValueError(f"parity 门禁不认识这个策略: {name}")

    return BacktestEngine(initial_capital=cap).run(request["symbol"], df, strategy)


# 门禁覆盖的策略（参数与 bench.py / server.cpp 的默认值一一对应）
GATED_STRATEGIES = [
    ("MA_CROSS", {"fast_period": 5, "slow_period": 20, "position_pct": 0.95}),
    ("MACD", {"fast_period": 12, "slow_period": 26, "signal_period": 9,
              "position_pct": 0.95}),
    ("KDJ", {"n": 9, "m1": 3, "m2": 3, "oversold": 20.0, "overbought": 80.0,
             "position_pct": 0.95}),
]
# 已登记的分歧：跑、报告、不参与门禁（理由见文件头）
DIAGNOSED_STRATEGIES = [
    ("RSI", {"period": 14, "oversold": 30.0, "overbought": 70.0,
             "position_pct": 0.95}),
]


def compare_one(bars: list[dict], request: dict) -> dict:
    """跑一个策略的两侧，返回逐项对比。不打印，不做判定。"""
    cpp = run_cpp(bars, request)["metrics"]
    py = run_python(bars, request)["metrics"]
    rows = []
    for label, get_py, get_cpp, note in MUST_MATCH:
        a, b = float(get_py(py)), float(get_cpp(cpp))
        diff = abs(a - b)
        rows.append({"metric": label, "python": a, "cpp": b,
                     "diff": diff, "ok": diff <= TOLERANCE, "note": note})
    return {"strategy": request["strategy"], "params": request["params"],
            "comparisons": rows, "python_metrics": py, "cpp_metrics": cpp}


def print_one(res: dict) -> list[str]:
    """打印一个策略的对比表，返回失败项。"""
    print(f"  ── {res['strategy']}  {res['params']}")
    print(f"  {'指标':<12}{'Python':>20}{'C++':>20}{'差值':>12}   换算")
    print("  " + "-" * 92)
    failures = []
    for r in res["comparisons"]:
        print(f"  {'✓' if r['ok'] else '✗'} {r['metric']:<10}{r['python']:>20.8f}"
              f"{r['cpp']:>20.8f}{r['diff']:>12.2e}   {r['note']}")
        if not r["ok"]:
            failures.append(f"{res['strategy']}/{r['metric']}: "
                            f"Python={r['python']} C++={r['cpp']} 差 {r['diff']:.3e}")
    print()
    return failures


def rsi_divergence_report(bars: list[dict]) -> dict:
    """量化 RSI 两侧的分歧，并指出根因。

    C++ 侧的逐根 bar 取值直接读指标金标准 fixture —— 那份 fixture 的
    real_180 用的就是本文件这 180 根 bar（backtest_engine/CMakeLists.txt 里
    GOLDEN_BARS_PATH 指向 benchmarks/data/cpp_bars.json），所以可以直接比。
    """
    import struct

    fx = HERE.parent / "backtest_engine" / "tests" / "data" / "indicator_golden.json"
    if not fx.exists():
        return {"available": False, "reason": f"指标金标准 fixture 不存在: {fx}"}

    rows = json.loads(fx.read_text())["series"]["real_180"]
    if len(rows) != len(bars):
        return {"available": False, "reason": "fixture 的 bar 数与本 fixture 不符"}

    def d(h: str) -> float:
        return struct.unpack("<d", struct.pack("<Q", int(h, 16)))[0]

    df = benchlib.add_rsi(benchlib.bars_to_df(bars).copy(), 14)
    py_series = [float(v) for v in df["rsi"].tolist()]
    cpp_series = [d(r["rsi14"]) for r in rows]

    start = 19          # 两侧都已过预热期（C++ 从 15 起决策，Python 从 19 起）
    worst, worst_at = 0.0, None
    for i in range(start, len(rows)):
        p_, c_ = py_series[i], cpp_series[i]
        if p_ != p_:
            continue
        if abs(c_ - p_) > worst:
            worst, worst_at = abs(c_ - p_), i

    # 偏差此后恒低于阈值的起点 —— 用来说明它是**衰减**的，不是恒定偏移
    def settles_below(thr: float):
        for i in range(start, len(rows)):
            if all(abs(cpp_series[j] - py_series[j]) < thr
                   for j in range(i, len(rows))):
                return i
        return None

    # 30/70 穿越事件两侧不一致的 bar —— 这才是它影响成交的直接原因
    disagreements = []
    for i in range(1, len(rows)):
        cp, cc = cpp_series[i - 1], cpp_series[i]
        pp, pc = py_series[i - 1], py_series[i]
        if pp != pp or pc != pc:
            continue
        c_ev = (cp < 30 <= cc) or (cp > 70 >= cc)
        p_ev = (pp < 30 <= pc) or (pp > 70 >= pc)
        if c_ev != p_ev:
            disagreements.append(i)

    return {
        "available": True,
        "max_abs_diff": worst,
        "max_abs_diff_at_bar": worst_at,
        "final_bar_diff": abs(cpp_series[-1] - py_series[-1]),
        "settles_below_1e-1_from_bar": settles_below(1e-1),
        "settles_below_1e-3_from_bar": settles_below(1e-3),
        "threshold_crossing_disagreements": disagreements,
        "cause": (
            "两侧算的是**两个不同定义的 RSI**，不是精度问题："
            "C++ 用经典 Wilder —— 前 period 个 change 先取 SMA 作种子，之后转递推；"
            "benchlib.add_rsi 用 pandas ewm(alpha=1/period, adjust=False)，"
            "从 **0** 起播种。两者之差按 (1-1/period)^n 衰减，"
            "半衰期约 9.4 根，到第 179 根已衰减到 2.5e-05 量级 —— "
            "也就是说这是一处**预热期定义分歧**，长序列上会自行收敛。"),
        "why_not_gated": (
            "要让它进门禁只有两条路：放宽容差，或改 benchlib 的 RSI 定义。"
            "前者毁掉门禁的全部意义，后者动了冻结的对照臂。所以选第三条："
            "查清、量化、登记在案，闸门一寸不让。"),
    }


def main() -> int:
    bars = json.loads((HERE / "data" / "cpp_bars.json").read_text())
    golden = json.loads((HERE / "data" / "cpp_parity_golden.json").read_text())
    base = golden["request"]

    print("=" * 78)
    print("  Parity 门禁 — Python 参照引擎 vs C++ 引擎")
    print("=" * 78)
    print(f"  数据    {len(bars)} 根真实日线  {bars[0]['date']} → {bars[-1]['date']}")
    print(f"  本金    {base['initial_capital']:,.0f}   市场 {base['market']}")
    print(f"  门禁    {', '.join(n for n, _ in GATED_STRATEGIES)}"
          f"   ·   已登记分歧  {', '.join(n for n, _ in DIAGNOSED_STRATEGIES)}")
    print()

    failures: list[str] = []
    gated: list[dict] = []

    for name, params in GATED_STRATEGIES:
        request = dict(base, strategy=name, params=params)
        try:
            res = compare_one(bars, request)
        except (urllib.error.URLError, OSError) as exc:
            print(f"❌ C++ 服务未响应（{CPP_URL}）：{exc}")
            print("   先启动：./backtest_engine/build/backtest_server 8002 &")
            return 2
        gated.append(res)
        failures += print_one(res)

    # ── 已登记的分歧：跑、报告、**不参与判定** ──
    diagnosed: list[dict] = []
    for name, params in DIAGNOSED_STRATEGIES:
        request = dict(base, strategy=name, params=params)
        res = compare_one(bars, request)
        worst = max(r["diff"] for r in res["comparisons"])
        res["max_diff"] = worst
        res["gated"] = False
        diagnosed.append(res)
        print(f"  ── {name}  已登记的分歧，不参与门禁")
        print(f"     七项里最大差 {worst:.2e}"
              f"（最终资产 Python {res['python_metrics']['final_value']:,.2f}"
              f" vs C++ {res['cpp_metrics']['final_value']:,.2f}）")

    rsi_div = rsi_divergence_report(bars)
    if rsi_div.get("available"):
        print(f"     根因：{rsi_div['cause']}")
        print(f"     实测：过预热期后最大差 {rsi_div['max_abs_diff']:.4f}"
              f"（第 {rsi_div['max_abs_diff_at_bar']} 根），"
              f"到最后一根衰减到 {rsi_div['final_bar_diff']:.2e}")
        print(f"           30/70 穿越事件两侧不一致的 bar："
              f"{rsi_div['threshold_crossing_disagreements']}"
              f" —— 成交时点不同就是从这里来的")
        print(f"     为什么不放进门禁：{rsi_div['why_not_gated']}")
    print()

    # ── 口径差异（在 MA_CROSS 那一臂上登记，它们是指标口径问题，与策略无关）──
    ma = gated[0]
    py, cpp = ma["python_metrics"], ma["cpp_metrics"]
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
        "input": {"bars": len(bars), "request": dict(base, strategy="(多策略)")},
        # ⚠️ 这个键保持指向 MA_CROSS 那一臂：benchmarks/make_report.py 一直读它，
        #    改成多策略的形状会把 README 里那张开篇的 parity 表打散。
        "comparisons": [{k: v for k, v in r.items() if k != "note"}
                        for r in gated[0]["comparisons"]],
        "gated_strategies": [
            {"strategy": g["strategy"], "params": g["params"],
             "max_diff": max(r["diff"] for r in g["comparisons"]),
             "comparisons": [{k: v for k, v in r.items() if k != "note"}
                             for r in g["comparisons"]]}
            for g in gated
        ],
        "diagnosed_divergences": [
            {"strategy": d["strategy"], "params": d["params"],
             "max_diff": d["max_diff"], "gated": False,
             "python_final_value": d["python_metrics"]["final_value"],
             "cpp_final_value": d["cpp_metrics"]["final_value"],
             "detail": rsi_div if d["strategy"] == "RSI" else None}
            for d in diagnosed
        ],
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

    print(f"  ✅ Parity 门禁通过 —— {len(gated)} 个策略的经济结果逐项一致，"
          f"可以跑性能对比。")
    print("     结果已写入 results/parity.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
