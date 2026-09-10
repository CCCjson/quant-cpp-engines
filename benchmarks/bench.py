#!/usr/bin/env python3
"""
bench.py — 回测引擎 benchmark 总驱动

三个口径，分开测、分开报（合成一个数字会掩盖真相）：

  A  纯引擎        Python: engine.run()（指标已备好，不计入）
                   C++:    进程内 BacktestEngine
  B  端到端        Python: 算指标 + engine.run()
                   C++:    同 A —— 它没有独立的「指标准备」阶段，指标在 on_bar 里算
  C  HTTP 路径     C++:    POST :8002/api/backtest/run（含 JSON 序列化 + HTTP 栈）

⚠️ C++ 在 A 和 B 里是**同一个数字**。这不是偷懒，是它的结构决定的 ——
   C++ 没有「先把指标算好再进循环」这个阶段。所以：
     · 拿 C++ vs Python-A 比，对 C++ 不利（它的循环里还扛着指标计算）
     · 拿 C++ vs Python-B 比，才是「同样的活，谁干得快」
   两个都报。

用法：
    conda run -n quant python benchmarks/bench.py            # 完整
    conda run -n quant python benchmarks/bench.py --smoke    # 小规模快速验证
"""

from __future__ import annotations

import argparse
import json
import pathlib
import platform
import subprocess
import sys
import time
import urllib.error
import urllib.request

from loguru import logger

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

# ⚠️ 关掉 Python 引擎的日志 sink。它每笔成交都 logger.info（engine.py:113），外加结束时的完整报告，
#    开着的话测的是终端 I/O 不是引擎。引擎源码一行没改。
#    「开日志慢多少」单独测（见 measure_logging_cost），不藏着。
logger.remove()

import benchlib  # noqa: E402
from python_reference import BacktestEngine  # noqa: E402
from python_reference.strategies import (  # noqa: E402
    KDJStrategy, MACDStrategy, MACrossStrategy, RSIStrategy,
)

CPP_BIN = ROOT / "backtest_engine" / "build" / "bench_backtest"
CPP_URL = "http://127.0.0.1:8002"

# 策略 → Python 侧构造器（参数与 bench_backtest.cpp 的 make_strategy 一一对应）
PY_STRATEGY = {
    "MA_CROSS": lambda: MACrossStrategy(5, 20, 0.95),
    "MACD":     lambda: MACDStrategy(0.95),
    "RSI":      lambda: RSIStrategy(30.0, 70.0, 0.95),
    "KDJ":      lambda: KDJStrategy(20.0, 80.0, 0.95),
}

STRATEGIES = list(PY_STRATEGY)
SIZES_FULL = [250, 1000, 2500, 10000, 25000]
SIZES_SMOKE = [250, 1000]
INITIAL_CAPITAL = 1_000_000.0
SYMBOL = "TEST.SH"


# ────────────────────────────────────────────────────────────
# Python 侧
# ────────────────────────────────────────────────────────────

def py_run(df, strategy_name: str) -> dict:
    """跑一次完整回测。引擎和策略都新建 —— 策略带跨 bar 状态，不能复用实例。"""
    engine = BacktestEngine(initial_capital=INITIAL_CAPITAL)
    return engine.run(SYMBOL, df, PY_STRATEGY[strategy_name]())


def bench_python(bars: list[dict], strategy: str, warmup: int, runs: int) -> dict:
    raw_df = benchlib.bars_to_df(bars)

    # ── 口径 A：指标预先算好，只测引擎循环 ──
    prepared = benchlib.prepare_indicators(raw_df, strategy)
    a = benchlib.timeit(lambda: py_run(prepared, strategy), warmup, runs)

    # ── 口径 B：端到端，指标准备也计入 ──
    def end_to_end():
        df = benchlib.prepare_indicators(raw_df, strategy)
        return py_run(df, strategy)

    b = benchlib.timeit(end_to_end, warmup, runs)

    # ── 指标准备单独计时 ──
    # ⛔ 不用 (端到端 − 纯引擎) 相减来推：指标准备是几十微秒量级，
    #    引擎是几十毫秒量级，相减的结果几乎全是运行间噪声 —— 实测会出现负数占比。
    #    直接测它自己。
    prep = benchlib.timeit(
        lambda: benchlib.prepare_indicators(raw_df, strategy), warmup, runs)

    result = py_run(prepared, strategy)
    m = result["metrics"]
    return {
        "engine": "python",
        "strategy": strategy,
        "bars": len(bars),
        "pure_engine": a,
        "end_to_end": b,
        "indicator_prep": prep,
        "indicator_prep_share": prep["median"] / b["median"] if b["median"] else 0.0,
        # 工作量核对
        "trades": int(m["num_trades"]),
        "final_value": float(m["final_value"]),
    }


# ────────────────────────────────────────────────────────────
# C++ 侧
# ────────────────────────────────────────────────────────────

def bench_cpp_inproc(bars_path: pathlib.Path, strategy: str, warmup: int, runs: int) -> dict:
    out = subprocess.run(
        [str(CPP_BIN), str(bars_path), strategy, str(warmup), str(runs)],
        capture_output=True, text=True, check=True,
    )
    data = json.loads(out.stdout)
    samples = sorted(data["samples"])
    data["timing"] = {
        "median": samples[len(samples) // 2],
        "p95": samples[min(len(samples) - 1, int(len(samples) * 0.95))],
        "min": samples[0],
        "max": samples[-1],
        "runs": len(samples),
    }
    del data["samples"]
    return data


def bench_cpp_http(bars: list[dict], strategy: str, warmup: int, runs: int) -> dict | None:
    body = json.dumps({
        "symbol": SYMBOL, "strategy": strategy, "params": {},
        "bars": bars, "initial_capital": INITIAL_CAPITAL, "market": "a_share",
    }).encode()

    def once():
        req = urllib.request.Request(
            f"{CPP_URL}/api/backtest/run", data=body, method="POST",
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read().decode())

    try:
        once()
    except (urllib.error.URLError, OSError):
        return None
    return benchlib.timeit(once, warmup, runs)


# ────────────────────────────────────────────────────────────
# 附加测量
# ────────────────────────────────────────────────────────────

def measure_logging_cost(bars: list[dict]) -> dict:
    """Python 引擎开着 loguru 慢多少。

    benchmark 全程是关日志跑的，那是合理的（不关就在测终端 I/O）。
    但「关了才快」这件事本身得说出来，不然读者有理由怀疑数字是挑出来的。
    """
    import io
    df = benchlib.prepare_indicators(benchlib.bars_to_df(bars), "MA_CROSS")

    off = benchlib.timeit(lambda: py_run(df, "MA_CROSS"), warmup=2, runs=5)

    sink = io.StringIO()
    handler = logger.add(sink, level="INFO")
    try:
        on = benchlib.timeit(lambda: py_run(df, "MA_CROSS"), warmup=2, runs=5)
    finally:
        logger.remove(handler)

    return {
        "bars": len(bars),
        "logging_off_median_s": off["median"],
        "logging_on_median_s": on["median"],
        "slowdown_x": on["median"] / off["median"] if off["median"] else 0.0,
        "note": "日志写内存 StringIO，不含终端渲染；写终端会更慢",
    }


def measure_cold_start() -> dict:
    """import pandas 的一次性成本。

    不计入引擎跑分（那不是引擎的成本），但它是调用方真实感受到的，所以单独登记。
    """
    code = "import time;t=time.perf_counter();import pandas;print(time.perf_counter()-t)"
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, check=True)
    return {"import_pandas_seconds": float(out.stdout.strip())}


def _cpp_build_flags() -> str:
    """
    从 CMakeCache.txt 里读实际的构建类型与优化选项。

    ⚠️ 这里原来是一个硬编码的字符串 "Release (-O2)"，而实际构建是 -O3。
    也就是说这份结果 JSON、以及由它生成的 benchmarks/README.md 里那一行
    环境说明，一直是错的。跑分报告里写错编译选项是很致命的一类错误——
    它不影响数字本身，但会让所有数字失去可信度。所以改成实测。
    """
    root = pathlib.Path(__file__).resolve().parent.parent
    for build_dir in ("build", "build-asan"):
        cache = root / "backtest_engine" / build_dir / "CMakeCache.txt"
        if not cache.exists():
            continue
        btype, flags = "", ""
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:"):
                btype = line.split("=", 1)[1].strip()
            elif line.startswith("CMAKE_CXX_FLAGS_RELEASE:") and btype.lower() == "release":
                flags = line.split("=", 1)[1].strip()
            elif line.startswith("CMAKE_CXX_FLAGS_DEBUG:") and btype.lower() == "debug":
                flags = line.split("=", 1)[1].strip()
        if btype:
            # 本项目自己加的选项（见 backtest_engine/CMakeLists.txt）
            own = "-Wall -Wextra -Werror -ffp-contract=off"
            return f"{btype} ({flags}) + {own}" if flags else f"{btype} + {own}"
    return "unknown (CMakeCache.txt not found; run ./build.sh first)"


def environment() -> dict:
    def _cmd(args: list[str]) -> str:
        try:
            return subprocess.run(args, capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return "unknown"

    import numpy
    import pandas
    return {
        "cpu": _cmd(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "cores": _cmd(["sysctl", "-n", "hw.ncpu"]),
        "os": f"{platform.system()} {platform.release()}",
        "python": platform.python_version(),
        "python_impl": platform.python_implementation(),
        "pandas": pandas.__version__,
        "numpy": numpy.__version__,
        "compiler": _cmd(["c++", "--version"]).splitlines()[0] if _cmd(["c++", "--version"]) != "unknown" else "unknown",
        "cpp_build_type": _cpp_build_flags(),
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
    }


# ────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true", help="小规模快速验证")
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--runs", type=int, default=10)
    args = ap.parse_args()

    if not CPP_BIN.exists():
        print(f"❌ 找不到 {CPP_BIN}")
        print("   先构建：cmake --build backtest_engine/build --target bench_backtest -j8")
        return 2

    sizes = SIZES_SMOKE if args.smoke else SIZES_FULL
    warmup, runs = (1, 3) if args.smoke else (args.warmup, args.runs)

    env = environment()
    print("=" * 96)
    print("  回测引擎 Benchmark — Python 参照引擎 vs C++ 引擎")
    print("=" * 96)
    for k, v in env.items():
        print(f"  {k:<16}{v}")
    print(f"\n  规模 {sizes}   策略 {STRATEGIES}   warmup {warmup} / runs {runs}")

    (HERE / "data").mkdir(exist_ok=True)
    (HERE / "results").mkdir(exist_ok=True)

    rows: list[dict] = []
    mismatches: list[str] = []

    for n in sizes:
        bars = benchlib.synth_bars(n)
        bars_path = HERE / "data" / f"synth_{n}.json"
        benchlib.write_json(str(bars_path), bars)

        print()
        print(f"─── {n} 根 bar " + "─" * 78)
        print(f"  {'策略':<10}{'C++ 进程内':>14}{'Py 纯引擎':>14}{'Py 端到端':>14}"
              f"{'C++ vs PyA':>12}{'C++ vs PyB':>12}{'工作量核对':>14}   （耗时取 min）")

        for s in STRATEGIES:
            cpp = bench_cpp_inproc(bars_path, s, warmup, runs)
            py = bench_python(bars, s, warmup, runs)

            # ⚠️ 主判据用 min 而不是 median。
            #    benchmark 的干扰是**单向**的 —— 系统活动只会让程序变慢，不会变快。
            #    所以 min 是「真实成本」的最佳估计，median 反而会被后台活动整体抬高。
            #    实测踩过：某一轮 KDJ@25000 的 max/min 到 5.52×，中位数被污染了 46%。
            #    median/p95 仍然写进结果文件，用来判断这一格干不干净。
            c_med = cpp["timing"]["min"]
            a_med = py["pure_engine"]["min"]
            b_med = py["end_to_end"]["min"]

            # 噪声标记：max/min 超过 1.5 说明这一格被干扰过，读数时要留意
            def _noisy(t):
                return t["max"] / t["min"] > 1.5 if t["min"] else False
            noisy = _noisy(cpp["timing"]) or _noisy(py["pure_engine"]) or _noisy(py["end_to_end"])

            # ── 工作量核对：两个引擎必须在做同一件事，否则速度对比无意义 ──
            #
            # ⚠️ 以**成交笔数**为主判据，不用净值。
            #    净值是 N 次交易复利的结果，两边任何一点数值差异都会被放大；
            #    而「做了多少笔交易」才真正决定引擎干了多少活 —— 那正是要对比的东西。
            #    净值用**相对**误差做辅助判据（绝对容差在 1e6 量级的数上没有意义）。
            #
            # ⚠️ bool(...) 不可省：py 侧的值是 numpy 标量，比较结果是 np.bool_，
            #    json.dump 会把它写成非法 JSON（实测过，结果文件直接解析不了）。
            py_fv = float(py["final_value"])
            rel = abs(cpp["final_value"] - py_fv) / max(abs(py_fv), 1.0)
            trades_match = int(cpp["trades"]) == int(py["trades"])
            same = bool(trades_match and rel < 1e-9)
            mark = "✓ 一致" if same else "✗ 不一致"
            if not same:
                kind = "成交笔数不同" if not trades_match else "笔数相同但净值有相对偏差"
                mismatches.append(
                    f"{s}@{n}: {kind} —— 成交 C++={cpp['trades']} Py={py['trades']}, "
                    f"净值 C++={cpp['final_value']:.6e} Py={py_fv:.6e} (相对差 {rel:.2e})"
                )

            print(f"  {s:<10}{c_med * 1000:>12.3f}ms{a_med * 1000:>12.3f}ms"
                  f"{b_med * 1000:>12.3f}ms{a_med / c_med:>11.1f}×{b_med / c_med:>11.1f}×"
                  f"{mark:>14}{'  ⚠噪声' if noisy else ''}")

            rows.append({
                "bars": n, "strategy": s,
                "cpp_inproc": cpp["timing"], "cpp_trades": cpp["trades"],
                "cpp_final_value": cpp["final_value"],
                "py_pure_engine": py["pure_engine"], "py_end_to_end": py["end_to_end"],
                "py_indicator_prep": py["indicator_prep"],
                "py_indicator_prep_share": py["indicator_prep_share"],
                "py_trades": int(py["trades"]), "py_final_value": float(py["final_value"]),
                "speedup_vs_py_pure": a_med / c_med,
                "speedup_vs_py_end_to_end": b_med / c_med,
                "workload_identical": same,
                "noisy": noisy,
                "trades_match": trades_match,
                "final_value_rel_diff": rel,
            })

    # ── HTTP 口径 ──
    print()
    print("─── HTTP 路径（含 JSON 序列化 + HTTP 栈） " + "─" * 52)
    http_rows = []
    probe_bars = benchlib.synth_bars(sizes[-1])
    for s in STRATEGIES:
        t = bench_cpp_http(probe_bars, s, warmup, runs)
        if t is None:
            print(f"  ⚠️  C++ 服务未跑在 {CPP_URL}，跳过 HTTP 口径")
            break
        inproc = next(r for r in rows if r["bars"] == sizes[-1] and r["strategy"] == s)
        # 同样取 min：两边都用最干净的那次，传输层开销才不会被噪声算成负数
        # （实测踩过：median 口径下 KDJ 这一格算出 −68.6% 的"负开销"）
        overhead = t["min"] - inproc["cpp_inproc"]["min"]
        print(f"  {s:<10}{t['min'] * 1000:>12.3f}ms   进程内 "
              f"{inproc['cpp_inproc']['min'] * 1000:>8.3f}ms   "
              f"传输层占 {overhead / t['min']:>5.1%}")
        http_rows.append({
            "strategy": s, "bars": sizes[-1], "http": t,
            "inproc_min": inproc["cpp_inproc"]["min"],
            "transport_overhead_s": overhead,
            "transport_share": overhead / t["min"],
        })

    # ── 附加测量 ──
    print()
    print("─── 附加测量 " + "─" * 80)
    log_cost = measure_logging_cost(benchlib.synth_bars(sizes[-1]))
    print(f"  Python 开日志     慢 {log_cost['slowdown_x']:.2f}×  "
          f"({log_cost['logging_off_median_s'] * 1000:.1f}ms → "
          f"{log_cost['logging_on_median_s'] * 1000:.1f}ms @ {log_cost['bars']} bar)")
    cold = measure_cold_start()
    print(f"  import pandas     {cold['import_pandas_seconds'] * 1000:.0f}ms（一次性，不计入引擎跑分）")

    payload = {
        "environment": env,
        "config": {"sizes": sizes, "strategies": STRATEGIES, "warmup": warmup, "runs": runs,
                   "initial_capital": INITIAL_CAPITAL, "symbol": SYMBOL},
        "results": rows,
        "http": http_rows,
        "logging_cost": log_cost,
        "cold_start": cold,
        "workload_mismatches": mismatches,
    }
    out_path = HERE / "results" / ("backtest_smoke.json" if args.smoke else "backtest.json")
    benchlib.write_json(str(out_path), payload)

    print()
    if mismatches:
        print("  ⚠️  工作量核对不一致（速度数字仍有效，但必须连同这个一起读）：")
        for m in mismatches:
            print(f"       · {m}")
    print(f"\n  结果已写入 {out_path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
