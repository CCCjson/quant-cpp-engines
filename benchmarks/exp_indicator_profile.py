#!/usr/bin/env python3
"""
exp_indicator_profile.py — 指标计算在整场回测里占多少时间

产出：
    results/indicator_profile.json

============================================================
测法，以及两次走错的路
============================================================

要回答的是「指标计算占整场回测的百分之几」。

⛔ 第一条走不通的路：把指标单拎出来跑个循环计时。
   那样测的是**数据全在 L1 里**的理想成本。真实回测中间还夹着组合估值、
   撮合、日历推进，会把指标要读的 bar 挤出缓存。孤立计时给的是下界，不是事实。

所以改用这个仓库已经在用的对照臂手法（与 MACD_NAIVE / MACD_NOALLOC 同源）：

    真实臂    S            —— 策略正常调用 ctx.macd() / ctx.sma() / ...
    对照臂    S_PRECOMP    —— 指标值预先算好，on_bar 只做一次数组下标取值，
                              其余每一行都不动

两条臂的成交笔数与最终净值必须**逐位相同**。相同，才说明工作量一致、
时间差真的只来自指标那一部分；不同，则本实验作废（脚本以非零码退出）。

⛔ 第二条走不通的路：两条臂分两次进程各测各的，然后相减。
   实测六个策略**全部落在噪声里**，其中 RSI 与 MOMENTUM 甚至测出
   **负的**指标成本 —— 对照臂做的事严格更少，不可能更慢。
   也就是说进程级抖动（地址布局、CPU 迁移、睿频与热节流）比要测的差值还大。
   那一版的数字一个都不能用，全部作废。

✅ 现在用的是**同进程内交替配对测量**（bench_backtest 的 PAIR: 模式）：
   同一个进程里交替跑 A、B，每轮得到一个差 d_i = t(A,i) − t(B,i)，
   共模漂移在相减时抵消。每轮还交换先后顺序，免得「先跑的承担缓存预热」
   固定偏向某一条臂。

============================================================
怎么判断「测出来了」
============================================================

不看「占比 X%」这个数好不好看，看**符号检验**：若指标成本真的存在，
d_i 应当几乎全为正。n 轮全正、纯属偶然的概率是 2^-n。
本脚本取 n=21，全正即 p ≈ 4.8e-07。

sign_p 没过 0.01 的行，占比数字不予采信，标为 inconclusive。

⚠️ 一个诚实的偏差方向：对照臂每根 bar 还要读一次预算好的数组，多摸一条缓存线。
   所以这里测出来的占比是**略偏低**的估计。

用法：
    ./build.sh
    cmake --build backtest_engine/build --target bench_backtest -j8
    python3 benchmarks/exp_indicator_profile.py

只依赖标准库。合成数据由 benchlib 用固定种子生成，逐位可复现。
"""

from __future__ import annotations

import json
import pathlib
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))
import benchlib  # noqa: E402

ROOT = HERE.parent
BENCH = ROOT / "backtest_engine" / "build" / "bench_backtest"
RESULTS = HERE / "results"
DATA = HERE / "data"

STRATEGIES = ["MA_CROSS", "MACD", "RSI", "KDJ", "BOLLINGER", "MOMENTUM"]
SIZES = [2500, 10000, 25000]

# 21 轮配对：全正时符号检验 p = 2^-21 ≈ 4.8e-07，足够把「测出来了」和
# 「没测出来」分开。再多的收益递减，而每轮要跑两场完整回测。
WARMUP, PAIRS = 5, 21
SIGN_ALPHA = 0.01


def ensure_data(n: int) -> pathlib.Path:
    p = DATA / f"synth_{n}.json"
    if not p.exists():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(benchlib.synth_bars(n)))
    return p


def run_pair(bars: pathlib.Path, strategy: str) -> dict:
    out = subprocess.run(
        [str(BENCH), str(bars), f"PAIR:{strategy}", str(WARMUP), str(PAIRS)],
        capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def sign_test_p(positive: int, n: int) -> float:
    """单尾符号检验：n 轮里有 positive 轮为正，全凭运气的概率。

    H0 是「两条臂没差别」，此时每轮为正的概率是 1/2。
    p = P(X >= positive)，X ~ Binomial(n, 1/2)。
    """
    from math import comb
    tail = sum(comb(n, k) for k in range(positive, n + 1))
    return tail / (2 ** n)


def main() -> int:
    if not BENCH.exists():
        print(f"找不到 {BENCH}，请先跑 ./build.sh 并构建 bench_backtest 目标",
              file=sys.stderr)
        return 1

    rows = []
    problems: list[str] = []

    for n in SIZES:
        bars = ensure_data(n)
        print(f"  {n:>6,} 根 bar ...", file=sys.stderr)
        for s in STRATEGIES:
            d = run_pair(bars, s)

            # ── 硬前提：两条臂必须在做同一件事 ──
            identical = (d["trades_with_indicator"] == d["trades_precomputed"]
                         and d["final_value_with_indicator"]
                         == d["final_value_precomputed"])
            if not identical:
                problems.append(
                    f"{n} 根 / {s}：真实臂与预算臂的经济结果不一致 "
                    f"（{d['trades_with_indicator']}笔/"
                    f"{d['final_value_with_indicator']!r} vs "
                    f"{d['trades_precomputed']}笔/"
                    f"{d['final_value_precomputed']!r}）")

            diffs = d["paired_diffs"]
            median_diff = statistics.median(diffs)
            # 分母取真实臂的 min：与本目录其它脚本的主统计量口径一致
            total = min(d["samples_with_indicator"])
            share = median_diff / total if total > 0 else 0.0
            p = sign_test_p(d["positive_diffs"], d["pairs"])

            rows.append({
                "bars": n,
                "strategy": s,
                "total_min": total,
                "median_paired_diff": median_diff,
                "indicator_share": share,
                "positive_diffs": d["positive_diffs"],
                "pairs": d["pairs"],
                "sign_p": p,
                "inconclusive": p > SIGN_ALPHA,
                "trades": d["trades_with_indicator"],
                "economics_identical": identical,
            })

    RESULTS.mkdir(exist_ok=True)
    out = {
        "_README": (
            "指标计算占整场回测的比例。真实臂正常调用指标，对照臂（*_PRECOMP）"
            "把指标值预先算好、只做数组取值，其余一行不动；两臂的 trades 与 "
            "final_value 必须逐位相同（economics_identical），否则本实验作废。"
            "两条臂在**同一个进程里交替**测量（bench_backtest 的 PAIR: 模式），"
            "每轮交换先后顺序，用配对差消掉共模漂移 —— 分进程各测各的那一版"
            "会测出负的指标成本，已废弃。"
            "indicator_share = 配对差中位数 / 真实臂 min。"
            "sign_p 是符号检验的 p 值；> 0.01 记为 inconclusive，那一行的占比不予采信。"
            "⚠️ 对照臂每根 bar 多读一次数组，所以这是**偏低**的估计。"
            "由 exp_indicator_profile.py 生成。"),
        "config": {
            "sizes": SIZES, "strategies": STRATEGIES,
            "warmup": WARMUP, "pairs": PAIRS, "sign_alpha": SIGN_ALPHA,
        },
        "results": rows,
    }
    (RESULTS / "indicator_profile.json").write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print("✓ 已写入 indicator_profile.json\n")
    print(f"{'规模':>8} {'策略':<11} {'整场(ms)':>10} {'指标(ms)':>10} "
          f"{'占比':>7} {'正号':>7} {'符号检验 p':>12}  判定")
    for r in rows:
        if not r["economics_identical"]:
            verdict = "❌ 两臂工作量不一致"
        elif r["inconclusive"]:
            verdict = "⚠️ 测不出来"
        else:
            verdict = "✅"
        print(f"{r['bars']:>8,} {r['strategy']:<11} "
              f"{r['total_min']*1000:>10.3f} "
              f"{r['median_paired_diff']*1000:>10.3f} "
              f"{r['indicator_share']*100:>6.1f}% "
              f"{r['positive_diffs']:>3}/{r['pairs']:<3} "
              f"{r['sign_p']:>12.2e}  {verdict}")

    if problems:
        print("\n❌ 对照臂不成立：", file=sys.stderr)
        for p in problems:
            print("   " + p, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
