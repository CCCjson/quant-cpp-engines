"""
benchlib.py — benchmark 的公共部件

三件事：
  1. 确定性合成行情（两个引擎喂同一份数据）
  2. 指标准备（Python 引擎需要预算好的列；公式对齐原项目的实现）
  3. 计时工具（报中位数/p95/min，不报均值）
"""

from __future__ import annotations

import json
import math
import statistics
import time
from typing import Any, Callable, Sequence

import pandas as pd


# ────────────────────────────────────────────────────────────
# 1. 确定性合成行情
# ────────────────────────────────────────────────────────────

# 正弦周期的振幅。这个值是**实测调出来的**，不是随手填的：
#   0.0040 → 25,000 根时策略赚到净值 3.3e11（本金的 33 万倍）。那个量级下
#            C++ 的 lot_floor 会触发 20 亿股钳位（strategy_context.h:80），
#            而 Python 没有钳位 → 两个引擎在大规模上分歧，工作量核对假报失败。
#   0.0012 → 25,000 根时亏到 −48%，仓位越缩越小，后段近乎空转。
#   0.0015 → 250 根 −1.2%（基本持平），25,000 根净值 2.2 倍本金。
#            **任意规模下都待在真实区间**，两端都不失真。
CYCLE_AMPLITUDE = 0.0015


def synth_bars(n: int, start: float = 100.0, seed: int = 7) -> list[dict]:
    """生成 n 根日线。

    用自己实现的线性同余随机数，不依赖 random 模块的实现细节——
    换机器、换 Python 版本，同一个 seed 产生的序列完全一致。
    这是「两个引擎喂同一份数据」的前提。

    ⚠️ 这里刻意用**均值回复**（OU 过程）而不是带漂移的随机游走。
       第一版用的是 `price *= 1 + drift + ...`，在 250 根上很正常，
       但 25,000 根复利下来价格冲到上万、账户净值到 1e17 量级 ——
       那既不像真实市场，又把浮点分歧放大到让两个引擎的结果对不上，
       于是「工作量核对」在大规模上假报不一致。
       均值回复能保证**任意 n 下价格都待在合理区间**，规模曲线才是干净的。

    走势 = 围绕基准价的均值回复 + 正弦周期 + 噪声。
    周期项保证均线/MACD/KDJ 这些穿越型策略真的会触发信号 ——
    纯随机游走可能一次都不触发，那样测的只是空转循环。
    """
    bars: list[dict] = []
    rng = seed
    x = 0.0                 # 对数偏离度，围绕 0 回复
    theta = 0.005           # 回复强度
    sigma = 0.008           # 噪声强度
    for i in range(n):
        rng = (rng * 1103515245 + 12345) % (2**31)
        noise = (rng / 2**31 - 0.5) * 2 * sigma
        cycle = math.sin(i / 9.0) * CYCLE_AMPLITUDE
        x += -theta * x + noise + cycle

        close = round(start * math.exp(x), 2)
        wick = abs(noise) + 0.003
        high = round(close * (1 + wick), 2)
        low = round(close * (1 - wick), 2)
        open_ = round((high + low) / 2, 2)
        bars.append({
            "date": _day_label(i),
            "open": open_, "high": high, "low": low, "close": close,
            "volume": 1_000_000 + (rng % 500_000),
        })
    return bars


def _day_label(i: int) -> str:
    """把序号映射成递增的 YYYY-MM-DD。

    两个引擎都只把 date 当**有序标签**用，不做日历运算（不判周末、不算自然日差），
    所以这里只需要保证严格递增且格式合法。每月按 28 天排，年份跟着进位。
    """
    year = 2020 + i // (12 * 28)
    rem = i % (12 * 28)
    month = 1 + rem // 28
    day = 1 + rem % 28
    return f"{year:04d}-{month:02d}-{day:02d}"


def bars_to_df(bars: Sequence[dict]) -> pd.DataFrame:
    """bars → DataFrame（date 作索引），这是 Python 引擎期望的输入形态。"""
    df = pd.DataFrame(list(bars))
    df["date"] = pd.to_datetime(df["date"])
    return df.set_index("date")


# ────────────────────────────────────────────────────────────
# 2. 指标准备
# ────────────────────────────────────────────────────────────
#
# ⚠️ 公平性关键点：这里**只算策略真正需要的那几列**。
#
# 原项目的入口是 AnalysisEngine.add_indicators(df)，它默认会算
# trend + momentum + volatility + volume 四大族（MA 全套、MACD、BOLL、ADX、
# RSI、KDJ、ATR、OBV……）。拿那个当 Python 的指标成本，等于凭空给 Python
# 加上一堆策略根本用不到的负担 —— 那样的对比对 Python 不公平。
#
# 公式逐行对齐原项目：
#   ma{N}              analysis_engine/indicators/trend.py:42
#   macd_dif/macd_dea  analysis_engine/indicators/trend.py:79-86
#   rsi                analysis_engine/indicators/momentum.py:40-52
#   kdj_k/kdj_d        analysis_engine/indicators/momentum.py:70-81

def add_ma(df: pd.DataFrame, periods: Sequence[int]) -> pd.DataFrame:
    for p in periods:
        df[f"ma{p}"] = df["close"].rolling(window=p).mean()
    return df


def add_macd(df: pd.DataFrame, fast: int = 12, slow: int = 26, signal: int = 9) -> pd.DataFrame:
    ema_fast = df["close"].ewm(span=fast, adjust=False).mean()
    ema_slow = df["close"].ewm(span=slow, adjust=False).mean()
    df["macd_dif"] = ema_fast - ema_slow
    df["macd_dea"] = df["macd_dif"].ewm(span=signal, adjust=False).mean()
    return df


def add_rsi(df: pd.DataFrame, period: int = 14) -> pd.DataFrame:
    delta = df["close"].diff()
    gain = delta.where(delta > 0, 0)
    loss = -delta.where(delta < 0, 0)
    avg_gain = gain.ewm(alpha=1 / period, adjust=False).mean()
    avg_loss = loss.ewm(alpha=1 / period, adjust=False).mean()
    df["rsi"] = 100 - (100 / (1 + avg_gain / avg_loss))
    return df


def add_kdj(df: pd.DataFrame, period: int = 9, m1: int = 3, m2: int = 3) -> pd.DataFrame:
    low_min = df["low"].rolling(window=period).min()
    high_max = df["high"].rolling(window=period).max()
    rsv = (100 * (df["close"] - low_min) / (high_max - low_min)).fillna(50)
    df["kdj_k"] = rsv.ewm(alpha=1 / m1, adjust=False).mean()
    df["kdj_d"] = df["kdj_k"].ewm(alpha=1 / m2, adjust=False).mean()
    return df


# 策略 → 它真正需要的指标列 + 准备函数
STRATEGY_INDICATORS: dict[str, Callable[[pd.DataFrame], pd.DataFrame]] = {
    "MA_CROSS": lambda df: add_ma(df, (5, 20)),
    "MACD": add_macd,
    "RSI": add_rsi,
    "KDJ": add_kdj,
}


def prepare_indicators(df: pd.DataFrame, strategy: str) -> pd.DataFrame:
    """只为指定策略准备它需要的列。返回新 df，不污染入参。"""
    return STRATEGY_INDICATORS[strategy](df.copy())


# ────────────────────────────────────────────────────────────
# 3. 计时
# ────────────────────────────────────────────────────────────

def timeit(fn: Callable[[], Any], warmup: int = 3, runs: int = 10) -> dict[str, float]:
    """跑 warmup 轮预热 + runs 轮正式计时，返回秒。

    ⛔ 不报均值：GC 停顿和 OS 调度抖动会把均值拉偏，而且偏的方向不可预测。
       中位数抗离群点，p95 反映尾部，min 是「机器最好状态下能到多快」。
    """
    for _ in range(warmup):
        fn()

    samples: list[float] = []
    for _ in range(runs):
        t0 = time.perf_counter()
        fn()
        samples.append(time.perf_counter() - t0)

    samples.sort()
    return {
        "median": statistics.median(samples),
        "p95": samples[min(len(samples) - 1, int(len(samples) * 0.95))],
        "min": samples[0],
        "max": samples[-1],
        "runs": runs,
    }


def write_json(path: str, obj: Any) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, ensure_ascii=False, indent=2)
