#!/usr/bin/env python3
"""
exp_machine_drift.py — 同一份未改动的代码，换一天重测会差多少

产出：
    results/machine_drift.json

============================================================
为什么需要这个实验
============================================================

本轮的预注册里有一条 P7：

    「接口重构之后，四个策略 @25,000 根全部落在**改动前**的 ±2% 以内」

写它的时候，「改动前」取的是 results/experiment_incremental_engine.json 里
记录的耗时 —— 那是**几天前**在这台机器上测的。

然后发现：把那份代码原样重测，一个字节都没改，四个策略比旧记录慢了
**2.9%–6.6%**（每个策略取三遍里最快的那遍；单遍最差到 +8.6%）。
过程中还撞见过一次 MA_CROSS 报 +30.4% —— 那遍机器正被占用，
紧接着的三遍都回到 +10% 以内，所以那个数是离群点，不是结论。

不管取哪个口径，它都**大于 ±2%**。也就是说 P7 那条容差根本不可能被满足，
不管重构做得多干净 —— 噪声比要测的效应还大。

这是预注册自己暴露出来的方法论缺陷，而且只有写下预测、再去验证才会暴露：
如果按常规做法「改完测一下，跟旧记录比比」，就会看到「慢了 8%」，
然后花时间去查一个根本不存在的性能回归。

所以这个脚本做两件事：

  1. **量出漂移本身**。这个数字应当公开 —— 它是所有跨天速度对比的
     精度上限。任何小于它的「提速」都不成立。
  2. **存一份同场基线**。改动前后必须在**同一次会话**里测，比较才有意义。

⚠️ 漂移的来源没有逐项拆解（睿频状态、温度、后台负载、内核版本、
   页面布局都可能贡献），本实验只量总量，不归因。

用法：
    ./build.sh
    cmake --build backtest_engine/build --target bench_backtest -j8
    python3 benchmarks/exp_machine_drift.py

只依赖标准库。
"""

from __future__ import annotations

import datetime
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

BARS = 25000
# RUNS 与旧记录当初用的一致（10），否则「同一个统计量」这个前提就不成立。
WARMUP, RUNS = 3, 10
# 整套重复 REPEATS 遍：漂移本身就不稳定 —— 实测过一次 MA_CROSS 报 +30.4%，
# 紧接着三遍都在 +10% 上下，那次是机器当时被占用。只测一遍会把离群点写成结论。
REPEATS = 3

# 旧记录里的字段名 → 策略名
RECORDED = {
    "MA_CROSS": "ma_cross_min",
    "MACD": "macd_min",
    "RSI": "rsi_min",
    "KDJ": "kdj_min",
}
# 这两个没有旧记录（本轮才第一次进 benchmark），只存今天的基线
BASELINE_ONLY = ["BOLLINGER", "MOMENTUM"]


def ensure_data(n: int) -> pathlib.Path:
    p = DATA / f"synth_{n}.json"
    if not p.exists():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(benchlib.synth_bars(n)))
    return p


def run(bars: pathlib.Path, strategy: str) -> float:
    out = subprocess.run([str(BENCH), str(bars), strategy, str(WARMUP), str(RUNS)],
                         capture_output=True, text=True, check=True).stdout
    return min(json.loads(out)["samples"])


def main() -> int:
    if not BENCH.exists():
        print(f"找不到 {BENCH}，请先跑 ./build.sh", file=sys.stderr)
        return 1

    prev_path = RESULTS / "experiment_incremental_engine.json"
    prev = json.loads(prev_path.read_text())["results"][-1]
    assert prev["bars"] == BARS, f"旧记录的最大规模不是 {BARS}"

    bars = ensure_data(BARS)
    rows = []

    # 每个策略测 REPEATS 遍，各遍都取 min；再看这几遍之间的散布
    samples: dict[str, list[float]] = {}
    for rep in range(REPEATS):
        print(f"  第 {rep + 1}/{REPEATS} 遍 ...", file=sys.stderr)
        for strategy in list(RECORDED) + BASELINE_ONLY:
            samples.setdefault(strategy, []).append(run(bars, strategy))

    for strategy, key in RECORDED.items():
        v = sorted(samples[strategy])
        old = prev[key]
        rows.append({
            "strategy": strategy,
            "recorded_min": old,
            # 主统计量取几遍里最快的那遍：与旧记录同口径（机器最好状态）
            "remeasured_min": v[0],
            "remeasured_worst": v[-1],
            "drift_pct": (v[0] / old - 1.0) * 100.0,
            "drift_pct_worst": (v[-1] / old - 1.0) * 100.0,
            "code_changed": False,
        })

    for strategy in BASELINE_ONLY:
        v = sorted(samples[strategy])
        rows.append({
            "strategy": strategy,
            "recorded_min": None,      # 本轮之前从未测过
            "remeasured_min": v[0],
            "remeasured_worst": v[-1],
            "drift_pct": None,
            "drift_pct_worst": None,
            "code_changed": False,
        })

    drifts = [abs(r["drift_pct"]) for r in rows if r["drift_pct"] is not None]

    RESULTS.mkdir(exist_ok=True)
    out = {
        "_README": (
            "同一份**未改动**的代码，换一天重测与旧记录的差。"
            "drift_pct = (今天 − 旧记录) / 旧记录。code_changed 恒为 false —— "
            "这些行之间唯一的变量就是时间。"
            "这个数字是所有**跨会话**速度对比的精度上限：小于它的「提速」不成立。"
            "同时 remeasured_min 是本次会话的**同场基线**，供改动后在同一台机器、"
            "同一次会话里对比用 —— 跨天比是无效的。"
            "⚠️ 漂移未做归因（睿频/温度/后台负载/内核/页面布局都可能贡献），只量总量。"
            "由 exp_machine_drift.py 生成。"),
        "measured_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "recorded_source": "results/experiment_incremental_engine.json",
        "config": {"bars": BARS, "warmup": WARMUP, "runs": RUNS, "repeats": REPEATS},
        "max_abs_drift_pct": max(drifts) if drifts else None,
        "min_abs_drift_pct": min(drifts) if drifts else None,
        "results": rows,
    }
    (RESULTS / "machine_drift.json").write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print("✓ 已写入 machine_drift.json\n")
    print(f"{'策略':<11} {'旧记录(ms)':>11} {'今天(ms)':>10} {'最好漂移':>9} "
          f"{'最差漂移':>9}  代码是否改动")
    for r in rows:
        old = f"{r['recorded_min']*1000:>11.3f}" if r["recorded_min"] else f"{'—':>11}"
        drift = f"{r['drift_pct']:>+8.1f}%" if r["drift_pct"] is not None else f"{'—':>9}"
        worst = (f"{r['drift_pct_worst']:>+8.1f}%"
                 if r["drift_pct_worst"] is not None else f"{'—':>9}")
        print(f"{r['strategy']:<11} {old} {r['remeasured_min']*1000:>10.3f} "
              f"{drift} {worst}  否")
    if drifts:
        print(f"\n跨会话漂移 {min(drifts):.1f}%–{max(drifts):.1f}% —— "
              f"小于这个幅度的「提速」不成立。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
