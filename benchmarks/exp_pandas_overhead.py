#!/usr/bin/env python3
"""
exp_pandas_overhead.py — H5「数据结构」实验的生成器

产出 results/experiment_pandas_overhead.json。

============================================================
这个实验在回答什么
============================================================

第一轮发现 Python 的逐 bar 循环慢。但「Python 慢」是个太笼统的说法 ——
究竟慢在解释器本身，还是慢在**数据结构用错了**？

拆开测每根 bar 里真正执行的那几个操作，并与 numpy 数组上的同一操作对照：

  df.iloc[i]['close']   取单值      —— 引擎每根 bar 都要做
  df.iloc[:i+1]         切历史窗口  —— 引擎每根 bar 都要做
  df['ma5'].iloc[-1]    读指标      —— 策略每根 bar 做约 4 次

DataFrame 是为**整列批量运算**设计的：每次标量索引都要走索引解析、类型分派、
构造返回对象。逐元素访问是它最差的用法。

结论（见 benchmarks/README 的 H5）：Python 引擎每根 bar 的成本里，
有六成多纯粹花在 DataFrame 的标量索引和切片上 —— 那部分不是 Python 的问题。

============================================================
为什么补这个脚本
============================================================

结果文件一直在仓库里，生成它的脚本从来没提交过。而报告开篇写着
「每个数字都能在 results/ 里找到出处」。现在补上。

用法：
    pip install -r benchmarks/requirements.txt
    python3 benchmarks/exp_pandas_overhead.py

⚠️ 结果文件里的 key 是带中文说明的长字符串（make_report.py 按原样查表），
所以这里的键名必须与既有文件**逐字符相同**，否则报告会 KeyError。
"""

from __future__ import annotations

import json
import pathlib
import sys
import timeit

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))

RESULTS = HERE / "results"
BARS = 25000

# ⚠️ 这些键名会被 make_report.py 按字面查表（含中文与双空格），不要改动
K_CLOSE = "engine: df.iloc[i]['close']  取当前收盘价"
K_OPEN = "engine: df.iloc[i]['open']   取成交价"
K_SLICE = "engine: df.iloc[:i+1]        切历史窗口"
K_MA5 = "strategy: df['ma5'].iloc[-1] 读指标（×4 次/bar）"
K_NP_CLOSE = "numpy: arr_close[i]"
K_NP_SLICE = "numpy: arr_close[:i+1]  （视图，零拷贝）"
K_NP_MA5 = "numpy: arr_ma5[i]"


def main() -> int:
    try:
        import numpy as np
        import pandas as pd
    except ImportError:
        print("需要 pandas / numpy：pip install -r benchmarks/requirements.txt",
              file=sys.stderr)
        return 1
    import benchlib

    bars = benchlib.synth_bars(BARS)
    df = benchlib.bars_to_df(bars)
    df = benchlib.prepare_indicators(df, "MA_CROSS")   # 需要 ma5 这一列
    arr_close = df["close"].to_numpy()
    arr_ma5 = df["ma5"].to_numpy()

    # 取靠后的下标，避免测到「窗口很小」的乐观情形
    i = len(df) - 2
    reps = 2000

    def us(stmt, glb) -> float:
        """返回单次操作的微秒数。取 min —— 干扰是单向的。"""
        t = min(timeit.repeat(stmt, globals=glb, number=reps, repeat=5))
        return t / reps * 1e6

    g = {"df": df, "i": i, "arr_close": arr_close, "arr_ma5": arr_ma5}
    per_op = {
        K_CLOSE: us("df.iloc[i]['close']", g),
        K_OPEN: us("df.iloc[i]['open']", g),
        K_SLICE: us("df.iloc[:i+1]", g),
        K_MA5: us("df['ma5'].iloc[-1]", g),
        K_NP_CLOSE: us("arr_close[i]", g),
        K_NP_SLICE: us("arr_close[:i+1]", g),
        K_NP_MA5: us("arr_ma5[i]", g),
    }

    # 每根 bar 的 pandas 开销：引擎每 bar 取 close/open 各一次、切一次窗口，
    # 策略每 bar 读约 4 次指标（见 python_reference 的实现）
    pandas_ops = (per_op[K_CLOSE] + per_op[K_OPEN] + per_op[K_SLICE]
                  + 4 * per_op[K_MA5])

    # 引擎每根 bar 的实测总成本：跑一遍纯引擎循环，除以 bar 数
    from python_reference import BacktestEngine
    from python_reference.strategies import MACrossStrategy
    from loguru import logger
    logger.remove()
    t = min(timeit.repeat(
        lambda: BacktestEngine(initial_capital=1_000_000).run("SYNTH", df, MACrossStrategy()),
        number=1, repeat=5))
    engine_total = t / BARS * 1e6

    out = {
        "bars": BARS,
        "per_op_us": per_op,
        "pandas_ops_per_bar_us": pandas_ops,
        "engine_total_per_bar_us": engine_total,
        "pandas_share": pandas_ops / engine_total,
        "_README": ("H5：把每根 bar 里的 DataFrame 标量索引/切片单独计时，"
                    "与 numpy 数组上的同一操作对照。由 exp_pandas_overhead.py 生成。"
                    "per_op_us 的键名含中文，make_report.py 按字面查表，不要改动。"),
        "environment": {"pandas": pd.__version__, "numpy": np.__version__,
                        "python": sys.version.split()[0]},
    }
    RESULTS.mkdir(exist_ok=True)
    (RESULTS / "experiment_pandas_overhead.json").write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n")
    print(f"✓ 已写入 {RESULTS / 'experiment_pandas_overhead.json'}")
    print(f"  每 bar 总成本 {engine_total:.1f} µs，其中 pandas 索引占 "
          f"{pandas_ops:.1f} µs（{pandas_ops / engine_total:.0%}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
