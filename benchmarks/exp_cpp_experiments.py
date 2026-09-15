#!/usr/bin/env python3
"""
exp_cpp_experiments.py — 三个 C++ 侧对照实验的生成器

产出：
    results/experiment_incremental.json   （朴素 O(N²) vs 增量 O(N)）
    results/experiment_allocation.json    （把「分配」与「算法」两个因子拆开）
    results/complexity_fit.json           （log-log 拟合斜率 = 复杂度阶数）

============================================================
为什么补这几个脚本
============================================================

这三个结果文件一直在仓库里，但**生成它们的脚本从来没提交过** —— 当初是用
一次性脚本跑出来的。而 benchmarks/README 开篇就写着「每个数字都能在 results/
里找到出处」。出处自己备不出来，对一个把方法论当卖点的报告是最伤的一处。

现在补齐。三个实验共用同一批 bench_backtest 的臂，所以放在一个脚本里：

    MACD_NAIVE        改动前那份 O(N²) 实现，冻结在 bench_backtest.cpp 里
    MACD_NOALLOC      仍然每根 bar 全量重算（仍是 O(N²)），但四个缓冲区复用同一块内存
    MACD_INCREMENTAL  手写的增量原型
    MACD / RSI / KDJ  引擎当前实现（已增量化）
    MA_CROSS          未改动的参照系

有了 NOALLOC 这条臂，才能把「不再重复分配」和「不再重复计算」这两个因子分开 ——
否则增量实验一次改掉了两件事，无法归因。

用法：
    ./build.sh
    python3 benchmarks/exp_cpp_experiments.py

只依赖标准库。合成数据由 benchlib 用固定种子生成，逐位可复现。
"""

from __future__ import annotations

import json
import math
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))
import benchlib  # noqa: E402

ROOT = HERE.parent
BENCH = ROOT / "backtest_engine" / "build" / "bench_backtest"
RESULTS = HERE / "results"
DATA = HERE / "data"

SIZES = [250, 1000, 2500, 10000, 25000]
WARMUP, RUNS = 3, 10


def ensure_data(n: int) -> pathlib.Path:
    p = DATA / f"synth_{n}.json"
    if not p.exists():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(benchlib.synth_bars(n)))
    return p


def run(bars: pathlib.Path, strategy: str) -> dict:
    out = subprocess.run([str(BENCH), str(bars), strategy, str(WARMUP), str(RUNS)],
                         capture_output=True, text=True, check=True).stdout
    d = json.loads(out)
    # 主统计量取 min：干扰是单向的，后台活动只会让程序变慢
    return {"min": min(d["samples"]), "trades": d["trades"], "final_value": d["final_value"]}


def loglog_slope(xs: list[int], ys: list[float]) -> float:
    """t ∝ N^k  →  log t = k·log N + c，最小二乘拟合出的 k 就是复杂度阶数。"""
    lx = [math.log(x) for x in xs]
    ly = [math.log(y) for y in ys]
    n = len(lx)
    mx, my = sum(lx) / n, sum(ly) / n
    num = sum((a - mx) * (b - my) for a, b in zip(lx, ly))
    den = sum((a - mx) ** 2 for a in lx)
    return num / den


def main() -> int:
    if not BENCH.exists():
        print(f"找不到 {BENCH}，请先跑 ./build.sh", file=sys.stderr)
        return 1

    # BOLLINGER 与 MOMENTUM 此前从未进过 benchmark —— 加进来是为了拟合它们的
    # 复杂度阶数（预注册 P4/P5，写在 results/prereg_2026-09-16.json 里）。
    arms = ["MACD_NAIVE", "MACD_NOALLOC", "MACD_INCREMENTAL",
            "MA_CROSS", "MACD", "RSI", "KDJ", "BOLLINGER", "MOMENTUM"]
    measured: dict[int, dict[str, dict]] = {}
    for n in SIZES:
        bars = ensure_data(n)
        print(f"  {n:>6,} 根 bar ...", file=sys.stderr)
        measured[n] = {a: run(bars, a) for a in arms}

    RESULTS.mkdir(exist_ok=True)
    problems: list[str] = []

    # ── 1. 增量实验 ──
    incremental = []
    for n in SIZES:
        m = measured[n]
        naive, incr, ma = m["MACD_NAIVE"], m["MACD_INCREMENTAL"], m["MA_CROSS"]
        identical = (naive["trades"] == incr["trades"]
                     and naive["final_value"] == incr["final_value"])
        if not identical:
            problems.append(f"{n} 根：朴素与增量的经济结果不一致")
        incremental.append({
            "bars": n,
            "macd_naive_min": naive["min"], "macd_incr_min": incr["min"],
            "ma_cross_min": ma["min"],
            "speedup": naive["min"] / incr["min"],
            "trades_naive": naive["trades"], "trades_incr": incr["trades"],
            "final_naive": naive["final_value"], "final_incr": incr["final_value"],
            "bitwise_identical": identical,
        })
    (RESULTS / "experiment_incremental.json").write_text(
        json.dumps(incremental, indent=2, ensure_ascii=False) + "\n")

    # ── 2. 分配 vs 算法 ──
    allocation = []
    for n in SIZES:
        m = measured[n]
        naive, noalloc, incr = m["MACD_NAIVE"], m["MACD_NOALLOC"], m["MACD_INCREMENTAL"]
        # 可优化空间 = naive − incremental；其中「消除分配」贡献 naive − noalloc
        total_gain = naive["min"] - incr["min"]
        alloc_gain = naive["min"] - noalloc["min"]
        all_identical = (naive["final_value"] == noalloc["final_value"]
                         == incr["final_value"])
        if not all_identical:
            problems.append(f"{n} 根：三条臂的最终净值不一致")
        allocation.append({
            "bars": n,
            "naive_min": naive["min"], "noalloc_min": noalloc["min"],
            "incremental_min": incr["min"],
            "alloc_share_of_gain": (alloc_gain / total_gain) if total_gain > 0 else 0.0,
            "algo_share_of_gain": 1.0 - ((alloc_gain / total_gain) if total_gain > 0 else 0.0),
            "all_identical": all_identical,
            "trades": naive["trades"],
        })
    (RESULTS / "experiment_allocation.json").write_text(
        json.dumps(allocation, indent=2, ensure_ascii=False) + "\n")

    # ── 3. 复杂度阶数拟合 ──
    #
    # Python 侧的斜率来自 backtest.json（那是 bench.py 的产物）；这里只拟合 C++ 侧，
    # 再把 Python 侧的合并进来，保持结果文件的形状不变。
    fit: dict[str, float] = {}
    #
    # 标签必须说清「这一行测的是哪个版本」。指标改成增量之后，C++/RSI 与
    # C++/KDJ 的斜率从 ~1.95 掉到 ~1.06 —— 如果标签还只写 "C++/RSI"，
    # 读者会以为 H2 那段「C++ 三个策略 k≈1.95」的结论被推翻了，
    # 而实际上那是**改动前**的测量。所以在标签里写明版本。
    for label, arm in [("C++/MACD（改动前 O(N²)）", "MACD_NAIVE"),
                       ("C++/MACD（增量原型）", "MACD_INCREMENTAL"),
                       ("C++/MACD（引擎当前）", "MACD"),
                       ("C++/MA_CROSS", "MA_CROSS"),
                       ("C++/RSI（引擎当前）", "RSI"),
                       ("C++/KDJ（引擎当前）", "KDJ"),
                       ("C++/BOLLINGER", "BOLLINGER"),
                       ("C++/MOMENTUM", "MOMENTUM")]:
        fit[label] = loglog_slope(SIZES, [measured[n][arm]["min"] for n in SIZES])

    bt_path = RESULTS / "backtest.json"
    if bt_path.exists():
        bt = json.loads(bt_path.read_text())
        for strat in {r["strategy"] for r in bt["results"]}:
            pts = sorted((r["bars"], r["py_pure_engine"]["min"])
                         for r in bt["results"] if r["strategy"] == strat)
            if len(pts) >= 2:
                fit[f"Python/{strat}"] = loglog_slope([p[0] for p in pts],
                                                      [p[1] for p in pts])
    (RESULTS / "complexity_fit.json").write_text(
        json.dumps(dict(sorted(fit.items())), indent=2, ensure_ascii=False) + "\n")

    print("✓ 已写入 experiment_incremental.json / experiment_allocation.json / complexity_fit.json")
    print(f"  改动前 MACD 斜率 k={fit['C++/MACD（改动前 O(N²)）']:.3f}（应≈2，即 O(N²)）")
    print(f"  增量版   斜率 k={fit['C++/MACD（增量原型）']:.3f}（应≈1，即 O(N)）")
    print(f"  引擎当前 RSI 斜率 k={fit['C++/RSI（引擎当前）']:.3f}，KDJ 斜率 k={fit['C++/KDJ（引擎当前）']:.3f}")

    if problems:
        for p in problems:
            print("❌ " + p, file=sys.stderr)
        return 1
    print("✅ 各臂经济结果一致，实验有效")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
