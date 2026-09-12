#!/usr/bin/env python3
"""
bench_py_version.py — 用「当前这个解释器」重跑 Python 侧，产出版本对照数据

产出 results/backtest_py313.json（文件名沿用历史，实际记录的是运行时的版本号）。

============================================================
这个实验在回答什么
============================================================

主跑分表的 Python 侧跑在 3.10（母项目的 conda 环境）。但 CPython 从 3.11 起做过
一轮大优化，只报一个版本对 Python 不公平。所以用另一个版本把 Python 侧完整重跑，
C++ 侧不动、数据与采样方式全部相同。

结论（见 benchmarks/README 的「换一个 Python 版本」一节）有两个方向相反的效果：
削弱了「语言」这个因子（C++ 的领先倍数下降），同时强化了核心结论
（MACD 上 C++ 输得更彻底）。而且这本身就说明：「语言/运行时」这个因子有
三成多的浮动，换个小版本号就变；而 O(N²)→O(N) 那两百多倍是结构性的。

============================================================
为什么补这个脚本
============================================================

结果文件一直在仓库里，生成它的脚本从来没提交过 —— 这是「每个数字都能找到出处」
这句话的最后一块缺口。

用法（在想要对照的那个解释器下跑）：
    pip install -r benchmarks/requirements.txt
    python3 benchmarks/bench_py_version.py

版本号从运行时读取并写进结果文件，所以换任何解释器跑都能自我描述。
"""

from __future__ import annotations

import json
import pathlib
import sys

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))

RESULTS = HERE / "results"
SIZES = [250, 1000, 2500, 10000, 25000]
STRATEGIES = ["MA_CROSS", "MACD", "RSI", "KDJ"]
WARMUP, RUNS = 3, 10
INITIAL_CAPITAL = 1_000_000
SYMBOL = "SYNTH"


def main() -> int:
    try:
        import numpy as np
        import pandas as pd
    except ImportError:
        print("需要 pandas / numpy：pip install -r benchmarks/requirements.txt",
              file=sys.stderr)
        return 1

    from loguru import logger
    logger.remove()   # 日志 I/O 不计入计时（与 bench.py 一致）

    import benchlib
    from python_reference import BacktestEngine
    from python_reference.strategies import (
        KDJStrategy, MACDStrategy, MACrossStrategy, RSIStrategy,
    )

    factory = {"MA_CROSS": MACrossStrategy, "MACD": MACDStrategy,
               "RSI": RSIStrategy, "KDJ": KDJStrategy}

    results = []
    for n in SIZES:
        bars = benchlib.synth_bars(n)
        raw = benchlib.bars_to_df(bars)
        for st in STRATEGIES:
            # 口径 A：指标预先算好，只测引擎循环 —— 与 bench.py 的 py_pure_engine 一致
            prepared = benchlib.prepare_indicators(raw, st)
            stats = benchlib.timeit(
                lambda: BacktestEngine(initial_capital=INITIAL_CAPITAL).run(
                    SYMBOL, prepared, factory[st]()),
                warmup=WARMUP, runs=RUNS)
            results.append({"bars": n, "strategy": st, "py_pure_engine": stats})
            print(f"  {n:>6,} 根 {st:<9} min={stats['min'] * 1000:.3f} ms",
                  file=sys.stderr)

    out = {
        "python": sys.version.split()[0],
        "pandas": pd.__version__,
        "numpy": np.__version__,
        "_README": ("Python 侧的版本对照数据：C++ 不变，只换解释器重跑。"
                    "由 bench_py_version.py 生成；版本号从运行时读取，"
                    "所以换任何解释器跑都能自我描述。"),
        "config": {"sizes": SIZES, "strategies": STRATEGIES,
                   "warmup": WARMUP, "runs": RUNS},
        "results": results,
    }
    RESULTS.mkdir(exist_ok=True)
    (RESULTS / "backtest_py313.json").write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n")
    print(f"✓ 已写入 {RESULTS / 'backtest_py313.json'}"
          f"（Python {out['python']} / pandas {out['pandas']}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
