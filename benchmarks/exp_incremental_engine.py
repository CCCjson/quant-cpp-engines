#!/usr/bin/env python3
"""
exp_incremental_engine.py — 「第三轮：修复与复测」的数据生成器

产出 results/experiment_incremental_engine.json。

============================================================
为什么需要这个脚本
============================================================

benchmarks/README 里那条核心结论 —— 「指标是 O(N²)，改成增量能提速两百多倍」——
最初是靠一个**未提交的一次性脚本**测出来的。结果文件在仓库里，生成它的东西不在。
对一个开篇就写「每个数字都能在 results/ 里找到出处」的报告，那是最伤的一处。

引擎里的 macd()/rsi()/kdj() 现在已经改成增量递推了，所以这一轮测的是**真实引擎**，
而不是一个写在 benchmark 里的原型：

  MACD_NAIVE  —— 改动前那份 O(N²) 实现，原样冻结在 bench_backtest.cpp 里作为对照臂。
                 不冻结它的话，改完之后 MACD 与 MACD_INCREMENTAL 就是同一个东西，
                 这个实验再也无法复现。
  MACD/RSI/KDJ —— 引擎当前的实现（已增量化）。
  MA_CROSS    —— 参照系。它只用 sma()，本次未改动，所以它的耗时应当基本不变；
                 三个指标策略修完之后应当回落到与它同一量级。

用法：
    ./build.sh                                   # 先构建（含 bench_backtest）
    python3 benchmarks/exp_incremental_engine.py

只依赖标准库。合成数据由 benchlib 用固定种子生成，任何机器上逐位可复现。
"""

from __future__ import annotations

import json
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

# 引擎当前实现 vs 冻结的改动前实现
ENGINE_ARMS = ["MA_CROSS", "MACD", "RSI", "KDJ"]
NAIVE_ARM = "MACD_NAIVE"


def ensure_data(n: int) -> pathlib.Path:
    p = DATA / f"synth_{n}.json"
    if not p.exists():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(benchlib.synth_bars(n)))
    return p


def run(bars: pathlib.Path, strategy: str) -> dict:
    out = subprocess.run(
        [str(BENCH), str(bars), strategy, str(WARMUP), str(RUNS)],
        capture_output=True, text=True, check=True,
    ).stdout
    d = json.loads(out)
    # 与 bench.py 一致：主统计量取 min。理由是干扰是单向的 ——
    # 后台活动只会让程序变慢，不会让它变快，所以 min 才是「真实成本」的最佳估计。
    return {
        "min": min(d["samples"]),
        "median": sorted(d["samples"])[len(d["samples"]) // 2],
        "max": max(d["samples"]),
        "trades": d["trades"],
        "final_value": d["final_value"],
    }


def main() -> int:
    if not BENCH.exists():
        print(f"找不到 {BENCH}，请先跑 ./build.sh", file=sys.stderr)
        return 1

    rows = []
    for n in SIZES:
        bars = ensure_data(n)
        print(f"  {n:>6,} 根 bar ...", file=sys.stderr)
        naive = run(bars, NAIVE_ARM)
        row = {"bars": n, "naive_min": naive["min"],
               "naive_trades": naive["trades"], "naive_final_value": naive["final_value"]}
        for arm in ENGINE_ARMS:
            r = run(bars, arm)
            row[f"{arm.lower()}_min"] = r["min"]
            row[f"{arm.lower()}_trades"] = r["trades"]
            row[f"{arm.lower()}_final_value"] = r["final_value"]
        row["macd_speedup"] = naive["min"] / row["macd_min"]
        # 经济结果必须逐位相同 —— 否则这不是同一个策略，提速数字没有意义
        row["economics_identical"] = (
            naive["trades"] == row["macd_trades"]
            and naive["final_value"] == row["macd_final_value"]
        )
        rows.append(row)

    out = {
        "_README": (
            "第三轮：修复与复测。MACD_NAIVE 是冻结在 bench_backtest.cpp 里的改动前 "
            "O(N^2) 实现；MACD/RSI/KDJ 是引擎当前的增量实现；MA_CROSS 是未改动的参照系。"
            "economics_identical 必须为 true —— 若为 false 则两条臂跑的不是同一个策略，"
            "提速数字没有意义。由 exp_incremental_engine.py 生成。"
        ),
        "config": {"sizes": SIZES, "warmup": WARMUP, "runs": RUNS,
                   "naive_arm": NAIVE_ARM, "engine_arms": ENGINE_ARMS},
        "results": rows,
    }
    RESULTS.mkdir(exist_ok=True)
    (RESULTS / "experiment_incremental_engine.json").write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n")
    print(f"✓ 已写入 {RESULTS / 'experiment_incremental_engine.json'}")

    bad = [r["bars"] for r in rows if not r["economics_identical"]]
    if bad:
        print(f"❌ 这些规模上经济结果不一致: {bad} —— 提速数字不可用", file=sys.stderr)
        return 1
    print("✅ 各规模上「改动前 vs 引擎当前」的经济结果逐位相同")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
