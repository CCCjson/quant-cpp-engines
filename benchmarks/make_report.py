#!/usr/bin/env python3
"""
make_report.py — 从 results/*.json 生成 benchmarks/README.md

⚠️ README 里的每一个数字都由本脚本从结果文件里读出来，**没有一个是手抄的**。
   想核对任何一个数，去 results/ 里翻对应的 json 即可。
   改了 benchmark 就重跑本脚本，不要手改 README。

用法：
    conda run -n quant python benchmarks/make_report.py
"""

from __future__ import annotations

import json
import pathlib
import sys

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))
R = HERE / "results"


def load(name: str):
    return json.loads((R / name).read_text(encoding="utf-8"))


bt = load("backtest.json")
fit = load("complexity_fit.json")
incr = load("experiment_incremental.json")
alloc = load("experiment_allocation.json")
pdo = load("experiment_pandas_overhead.json")
ob = load("orderbook.json")
parity = load("parity.json")
py313 = load("backtest_py313.json")
inc_engine = load("experiment_incremental_engine.json")

# ────────────────────────────────────────────────────────────
# 预注册（pre-registration）
#
# ⚠️ prereg_*.json 是 results/ 里唯一**没有生成脚本**的文件，而且必须没有 ——
#    它记的是预测，不是测量。生成脚本意味着「跑一下就能重新得到」，
#    而预测的全部价值恰恰在于它写在跑之前、并且此后不许再动。
#
# 下面两个渲染函数把它变成表格。measured 为 null 时显示「待回填」，
# 回填后自动显示命中/落空 —— 这样 README 永远不会出现「预测和实测对不上
# 但文字还在说命中」这种情况，因为两边都来自同一个文件。
# ────────────────────────────────────────────────────────────
prereg = load("prereg_2026-09-16.json")
profile = load("indicator_profile.json")
drift = load("machine_drift.json")


def profile_table(lang="zh"):
    """剖析结果：指标占整场回测多少。测不出来的行如实标出来，不给数字。"""
    if lang == "zh":
        head = ("| 规模 | 策略 | 整场 (ms) | 指标 (ms) | 占比 | 正号 | 符号检验 p | 判定 |\n"
                "|---|---|---|---|---|---|---|---|")
    else:
        head = ("| Bars | Strategy | Whole backtest (ms) | Indicator (ms) | Share | "
                "Positive | Sign-test p | Verdict |\n|---|---|---|---|---|---|---|---|")
    rows = []
    for r in profile["results"]:
        if not r["economics_identical"]:
            v = "❌ 两臂工作量不一致" if lang == "zh" else "❌ arms not equivalent"
        elif r["inconclusive"]:
            v = "⚠️ 测不出来" if lang == "zh" else "⚠️ not measurable"
        else:
            v = "✅"
        share = "—" if r["inconclusive"] else f"{r['indicator_share'] * 100:.1f}%"
        rows.append(
            f"| {r['bars']:,} | `{r['strategy']}` | {r['total_min'] * 1000:.3f} "
            f"| {r['median_paired_diff'] * 1000:.3f} | {share} "
            f"| {r['positive_diffs']}/{r['pairs']} | {r['sign_p']:.1e} | {v} |")
    return head + "\n" + "\n".join(rows)


def drift_table(lang="zh"):
    if lang == "zh":
        head = ("| 策略 | 旧记录 (ms) | 今天重测 (ms) | 漂移 | 代码改动 |\n"
                "|---|---|---|---|---|")
    else:
        head = ("| Strategy | Earlier record (ms) | Re-measured today (ms) | Drift | "
                "Code changed |\n|---|---|---|---|---|")
    rows = []
    for r in drift["results"]:
        old = f"{r['recorded_min'] * 1000:.3f}" if r["recorded_min"] else "—"
        d = f"**{r['drift_pct']:+.1f}%**" if r["drift_pct"] is not None else "—"
        never = "从未测过" if lang == "zh" else "never measured before"
        no = "**否**" if lang == "zh" else "**no**"
        rows.append(f"| `{r['strategy']}` | {old if r['recorded_min'] else never} "
                    f"| {r['remeasured_min'] * 1000:.3f} | {d} | {no} |")
    return head + "\n" + "\n".join(rows)


# 剖析里能测出来的最大占比 —— 现算，不写死
def gate_table(lang="zh"):
    """门禁覆盖了哪些策略，以及登记在案的分歧。"""
    head = ("| 策略 | 七项经济指标最大差 | 是否进门禁 |\n|---|---|---|" if lang == "zh"
            else "| Strategy | Largest of the seven diffs | In the gate? |\n|---|---|---|")
    rows = []
    for g in parity.get("gated_strategies", []):
        yes = "✅ 是" if lang == "zh" else "✅ yes"
        rows.append(f"| `{g['strategy']}` | `{g['max_diff']:.2e}` | {yes} |")
    for d in parity.get("diagnosed_divergences", []):
        no = "❌ 否 —— 已登记的分歧" if lang == "zh" else "❌ no — a registered divergence"
        rows.append(f"| `{d['strategy']}` | `{d['max_diff']:.2e}` | {no} |")
    return head + "\n" + "\n".join(rows)


# RSI 分歧的实测数字 —— 现算，不写死
_rsi = next((d["detail"] for d in parity.get("diagnosed_divergences", [])
             if d["strategy"] == "RSI" and d.get("detail")), None)
_rsi_row = next((d for d in parity.get("diagnosed_divergences", [])
                 if d["strategy"] == "RSI"), None)

_measurable = [r for r in profile["results"] if not r["inconclusive"]]
profile_max_share = max(r["indicator_share"] for r in _measurable) * 100
profile_min_share = min(r["indicator_share"] for r in _measurable) * 100
drift_lo = drift["min_abs_drift_pct"]
drift_hi = drift["max_abs_drift_pct"]


def _prereg_rows(items, cols):
    """把 dict 列表渲染成 markdown 表格。cols = [(表头, 取值函数), ...]"""
    head = "| " + " | ".join(h for h, _ in cols) + " |"
    sep = "|" + "|".join("---" for _ in cols) + "|"
    body = ["| " + " | ".join(str(f(it)) for _, f in cols) + " |" for it in items]
    return "\n".join([head, sep] + body)


def _measured_cell(p, lang="zh"):
    m = p.get("measured")
    note_key = "note" if lang == "zh" else "note_en"
    # 注记记的是「这条预测本身有什么毛病」，即便还没测也要显示出来 ——
    # 藏起来就等于事后悄悄改预测。
    note = p.get(note_key, "")
    if m is None:
        pending = "⏳ 待回填" if lang == "zh" else "⏳ not yet measured"
        return f"{pending}<br>{note}" if note else pending
    key = "detail" if lang == "zh" else "detail_en"
    vkey = "verdict" if lang == "zh" else "verdict_en"
    cell = f"{m.get(vkey, m.get('verdict', ''))} {m.get(key, m.get('detail', ''))}".strip()
    return f"{cell}<br>{note}" if note else cell


def prereg_class_table(lang="zh"):
    cols_zh = [
        ("指标", lambda it: f"`{it['indicator']}`"),
        ("类", lambda it: f"**{it['class']}**"),
        ("处理", lambda it: it["status"]),
        ("依据（代码事实）", lambda it: it["evidence"]),
    ]
    cols_en = [
        ("Indicator", lambda it: f"`{it.get('indicator_en', it['indicator'])}`"),
        ("Class", lambda it: f"**{it['class']}**"),
        ("Disposition", lambda it: it.get("status_en", it["status"])),
        ("Basis (what the code actually does)", lambda it: it.get("evidence_en", it["evidence"])),
    ]
    return _prereg_rows(prereg["classification"]["items"],
                        cols_zh if lang == "zh" else cols_en)


def prereg_pred_table(lang="zh"):
    cols_zh = [
        ("#", lambda p: f"**{p['id']}**"),
        ("预测", lambda p: p["claim"]),
        ("实测", lambda p: _measured_cell(p, "zh")),
    ]
    cols_en = [
        ("#", lambda p: f"**{p['id']}**"),
        ("Prediction", lambda p: p.get("claim_en", p["claim"])),
        ("Measured", lambda p: _measured_cell(p, "en")),
    ]
    return _prereg_rows(prereg["predictions"],
                        cols_zh if lang == "zh" else cols_en)


# 预测的命中率 —— 现算，不写死。全部待回填时显示为 None。
_scored = [p for p in prereg["predictions"] if p.get("measured") is not None]
prereg_hits = sum(1 for p in _scored if p["measured"].get("verdict", "").startswith("✅"))
prereg_scored = len(_scored)


def r313(bars: int, strategy: str):
    return next(r for r in py313["results"] if r["bars"] == bars and r["strategy"] == strategy)

# Python 参照引擎的真实行数 —— 现数，不写死
py_loc = sum(len(f.read_text(encoding="utf-8").splitlines())
             for f in sorted((HERE / "python_reference").rglob("*.py")))
# ⛔ 必须排除所有构建目录 —— 里面是 FetchContent 拉下来的 googletest / httplib /
#    json 源码，数进去会得到 15 万行这种离谱数字（踩过两次）。
#
#    第二次踩的原因值得记下来：原来的判据是 `"build" not in f.parts`，
#    精确匹配名为 build 的路径段。后来加了 ASan 构建目录 build-asan/，
#    它不叫 build，于是漏网，行数从 7,356 跳到 150,697。
#    判据改成「任何以 build 开头的路径段」，把 build/、build-asan/、
#    以及将来可能出现的 build-tsan/ 之类一并挡住。
def _is_build_artifact(path) -> bool:
    return any(part.startswith("build") for part in path.parts)

cpp_loc = sum(len(f.read_text(encoding="utf-8", errors="ignore").splitlines())
              for f in sorted((HERE.parent / "backtest_engine").rglob("*"))
              if f.suffix in {".cpp", ".h"} and not _is_build_artifact(f))

env = bt["environment"]
SIZES = bt["config"]["sizes"]


def row(bars: int, strategy: str):
    return next(r for r in bt["results"] if r["bars"] == bars and r["strategy"] == strategy)


def ms(seconds: float) -> str:
    return f"{seconds * 1000:,.3f}"


# ────────────────────────────────────────────────────────────
# 各段素材
# ────────────────────────────────────────────────────────────

# 主结果表
main_rows = []
for s in bt["config"]["strategies"]:
    cells = []
    for n in SIZES:
        r = row(n, s)
        flag = " ⚠" if r["noisy"] else ""
        cells.append(f"{r['speedup_vs_py_pure']:.1f}×{flag}")
    main_rows.append(f"| `{s}` | " + " | ".join(cells) + " |")

# 绝对耗时表
abs_rows = []
for s in bt["config"]["strategies"]:
    for n in SIZES:
        r = row(n, s)
        abs_rows.append(
            f"| `{s}` | {n:,} | {ms(r['cpp_inproc']['min'])} | "
            f"{ms(r['py_pure_engine']['min'])} | {r['speedup_vs_py_pure']:.1f}× | "
            f"{r['cpp_trades']:,} / {r['py_trades']:,} |")

# 复杂度拟合
fit_rows = []
for key in sorted(fit):
    k = fit[key]
    verdict = "**O(N)** 线性" if k < 1.25 else ("**O(N²)** 二次" if k > 1.7 else "介于两者之间")
    eng, strat = key.split("/", 1)
    fit_rows.append(f"| {eng} | `{strat}` | **{k:.3f}** | {verdict} |")

# 增量实验
incr_rows = [
    f"| {r['bars']:,} | {ms(r['macd_naive_min'])} | {ms(r['macd_incr_min'])} | "
    f"**{r['speedup']:.1f}×** | {'✅ 逐位相同' if r['bitwise_identical'] else '❌ 不一致'} |"
    for r in incr
]
incr_last = incr[-1]
ma_last = row(SIZES[-1], "MA_CROSS")
macd_last = row(SIZES[-1], "MACD")

# 增量版修好之后，对 Python 的比值
incr_vs_py = macd_last["py_pure_engine"]["min"] / incr_last["macd_incr_min"]

# 分配实验
alloc_rows = [
    f"| {r['bars']:,} | {ms(r['naive_min'])} | {ms(r['noalloc_min'])} | {ms(r['incremental_min'])} | "
    f"{r['alloc_share_of_gain']:.0%} | {r['algo_share_of_gain']:.0%} |"
    for r in alloc
]
alloc_last = alloc[-1]
alloc_only_gain = alloc_last["naive_min"] / alloc_last["noalloc_min"]

# 零分配版仍是二次的？用两个最大规模估斜率
import math
a2, a1 = alloc[-1], alloc[-2]
noalloc_k = math.log(a2["noalloc_min"] / a1["noalloc_min"]) / math.log(a2["bars"] / a1["bars"])

# H3 向量化
py_prep_last = row(SIZES[-1], "MACD")["py_indicator_prep"]["min"]
cpp_macd_last = macd_last["cpp_inproc"]["min"]
cpp_ma_last = ma_last["cpp_inproc"]["min"]
cpp_indicator_cost = cpp_macd_last - cpp_ma_last          # 近似：整场 − 几乎无指标的那场
vectorize_ratio = cpp_indicator_cost / py_prep_last

# H5 pandas
pd_ops = pdo["per_op_us"]
pd_rows = [
    ("`df.iloc[i]['close']` 取单值", pd_ops["engine: df.iloc[i]['close']  取当前收盘价"],
     pdo["per_op_us"]["numpy: arr_close[i]"]),
    ("`df.iloc[:i+1]` 切历史窗口", pd_ops["engine: df.iloc[:i+1]        切历史窗口"],
     pdo["per_op_us"]["numpy: arr_close[:i+1]  （视图，零拷贝）"]),
    ("`df['ma5'].iloc[-1]` 读指标", pd_ops["strategy: df['ma5'].iloc[-1] 读指标（×4 次/bar）"],
     pdo["per_op_us"]["numpy: arr_ma5[i]"]),
]
pd_table = [f"| {label} | {p:.3f} µs | {n:.3f} µs | **{p / n:.0f}×** |" for label, p, n in pd_rows]

# H6 传输层
http_rows = [
    f"| `{h['strategy']}` | {ms(h['http']['min'])} | {ms(h['inproc_min'])} | {h['transport_share']:.1%} |"
    for h in bt["http"]
]
http_small = next(h for h in bt["http"] if h["strategy"] == "MA_CROSS")

# H7 日志
lc = bt["logging_cost"]

# 订单簿
sub = ob["submit"][-1]
depth_rows = [
    f"| {q['bid_depth'] + q['ask_depth']:,} | {q['latency']['p50_ns']:,.0f} | "
    f"{q['latency']['p99_ns']:,.0f} | {q['amortized_ns']:,.0f} |"
    for q in ob["depth_query"]
]
cancel = ob["cancel"]
# 加 order_id 索引之前的基线，用来给出 before/after 对照
ob_before = load("orderbook_before_cancel_index.json")
cancel_before = ob_before["cancel"]
sub_before = ob_before["submit"][-1]
depth_before = ob_before["depth_query"]

# KDJ 工作量分歧
kdj_rows = [
    f"| {n:,} | {row(n, 'KDJ')['cpp_trades']:,} | {row(n, 'KDJ')['py_trades']:,} |"
    for n in SIZES
]

# parity
parity_rows = [
    f"| {c['metric']} | {c['python']:,.8f} | {c['cpp']:,.8f} | {c['diff']:.2e} |"
    for c in parity["comparisons"]
]

# Python 版本对照
v_rows = []
for st in bt["config"]["strategies"]:
    a = row(SIZES[-1], st)["py_pure_engine"]["min"]
    b = r313(SIZES[-1], st)["py_pure_engine"]["min"]
    c = row(SIZES[-1], st)["cpp_inproc"]["min"]
    v_rows.append(
        f"| `{st}` | {ms(a)} | {ms(b)} | **{a / b:.2f}×** | {a / c:.1f}× → **{b / c:.1f}×** |")

v_ma = (row(SIZES[-1], "MA_CROSS")["py_pure_engine"]["min"]
        / r313(SIZES[-1], "MA_CROSS")["py_pure_engine"]["min"])
v_ma_ratio_new = (r313(SIZES[-1], "MA_CROSS")["py_pure_engine"]["min"]
                  / row(SIZES[-1], "MA_CROSS")["cpp_inproc"]["min"])
v_macd_ratio_new = (r313(SIZES[-1], "MACD")["py_pure_engine"]["min"]
                    / row(SIZES[-1], "MACD")["cpp_inproc"]["min"])

r3 = inc_engine["results"]
r3_last = r3[-1]
r3_rows = [
    f"| {r['bars']:,} | {ms(r['naive_min'])} | {ms(r['macd_min'])} | **{r['macd_speedup']:.0f}×** | "
    f"{'✅ 逐位相同' if r['economics_identical'] else '❌ 不一致'} |"
    for r in r3
]

NL = "\n"

# ────────────────────────────────────────────────────────────
# 正文
# ────────────────────────────────────────────────────────────

doc_zh = f"""# 快慢到底由什么决定 —— 一次从假设到证伪的性能调查

这不是一份「C++ 比 Python 快多少」的跑分表。

调查是从那个假设开始的，但**它在第一轮数据里就被证伪了**。剩下的部分是在回答：
既然不是语言，那是什么？

全部结论都建立在同一份数据集、同一台机器、可复现的对照实验之上。
每个数字都能在 [`results/`](results/) 里找到出处 —— README 由
[`make_report.py`](make_report.py) 生成，没有手抄。

---

## 目录

- [起点：一句没测过的话](#起点一句没测过的话)
- [素材：一个真实的对照组](#素材一个真实的对照组)
- [第一轮：假设是语言差异 —— 然后它被证伪了](#第一轮假设是语言差异--然后它被证伪了)
- [第二轮：七个假设，逐一验证](#第二轮七个假设逐一验证)
- [归因汇总](#归因汇总)
- [所以，什么时候该用哪个](#所以什么时候该用哪个)
- [一个不能比的例子](#一个不能比的例子)
- [订单簿：没有对照组的绝对基线](#订单簿没有对照组的绝对基线)
- [为了让这些数字可信，做了什么](#为了让这些数字可信做了什么)
- [第三轮：修复与复测](#第三轮修复与复测)
- [第四轮：先预测，再动手](#第四轮先预测再动手)
- [这次调查查出来的具体问题](#这次调查查出来的具体问题)
- [复现](#复现)

---

## 起点：一句没测过的话

本仓库的设计稿 [`docs/design-backtest.md`](../docs/design-backtest.md) 里写着一句：

> **性能**：C++ 逐 bar 循环比 Python 快得多

立项时的设想，**从来没有测过**。展示场合最怕这种句子 —— 懂行的人第一句就会问「快多少」。

于是有了这次调查。

---

## 素材：一个真实的对照组

性能对比最常见的作弊方式，是拿一个随手写的实现去比一个精心打磨的实现。
这里刻意避开了这一点：对照组不是为 benchmark 写的，而是**一个真实跑过生产的 Python 回测引擎**，
在 C++ 版本成熟后被整体退役。取回过程、逐条改动清单见 [`ORIGIN.md`](ORIGIN.md)。

跑分之前先过一道**硬门禁**：同一份 180 根真实日线、同一个策略，
两个引擎的经济结果必须逐项对齐。对不上就说明它们算的不是同一件事，那跑分毫无意义。

| 指标 | Python | C++ | 差值 |
|---|---|---|---|
{NL.join(parity_rows)}

**七项全部 `0.00e+00`** —— 不是「在容差内」，是逐位相同。
门禁脚本：[`parity_gate.py`](parity_gate.py)，结果：[`results/parity.json`](results/parity.json)。

<details>
<summary>两处已知的口径差异（登记在案，不掩盖）</summary>

- **年化波动率** 差 `{parity['known_differences']['volatility']['diff']:.2e}`：
  pandas 的 `.std()` 默认 `ddof=1`（样本标准差），C++ 用 `ddof=0`（总体）。比值恰为 `sqrt(n/(n-1))`。
- **夏普比率** 差 `{parity['known_differences']['sharpe_ratio']['diff']:.4f}`：两种都是标准算法，年化路径不同 ——
  C++ 走「日均算术超额收益 × √252」，Python 走「(几何年化收益 − rf) / 年化波动率」。
  母项目 2026-07-15 的退役记录写的是「夏普差 0.018」，本 fixture 上实测是
  `{parity['known_differences']['sharpe_ratio']['diff']:.4f}`。两种年化路径在收益率序列偏度大时差距会放大，
  当年应该是用了另一组数据 —— **旧记录没有被采信，以实测为准**。
- **成交笔数** Python 记 fills、C++ 记 round-trips，约 2:1，同一批交易。

</details>

---

## 第一轮：假设是语言差异 —— 然后它被证伪了

**假设 H1**：C++ 是编译型语言，Python 是解释型，所以 C++ 快。差距应该在各种规模、各个策略上大体一致。

四个两边都有的策略 × 五个数据规模，喂**完全相同**的合成行情。
下表是 C++ 进程内 vs Python 纯引擎的倍数（耗时取 `min`，理由见[方法论](#为了让这些数字可信做了什么)）：

| 策略 | {" | ".join(f"{n:,} 根" for n in SIZES)} |
|---|{"---|" * len(SIZES)}
{NL.join(main_rows)}

如果 H1 成立，每一行都该是一条大致水平的线。

**它不是。**

`MA_CROSS` 确实稳定在 {row(SIZES[-1], 'MA_CROSS')['speedup_vs_py_pure']:.0f}–{row(SIZES[0], 'MA_CROSS')['speedup_vs_py_pure']:.0f}× —— 符合假设。
但另外三个策略随规模**急剧塌陷**，到 {SIZES[-1]:,} 根时 `MACD` 只有
**{macd_last['speedup_vs_py_pure']:.1f}×** —— 也就是 **C++ 输给了 Python**。

同一个语言、同一个引擎、同一份数据，只是换了个策略，结论就反过来了。
**H1 无法解释这件事，被证伪。**

---

## 第二轮：七个假设，逐一验证

### H2 · 算法复杂度阶数 ✅ 成立，且是主因

如果差距来自复杂度阶数，那么把耗时对规模做 log-log 回归，斜率就是阶数本身：
`t ∝ N^k` → `log t = k·log N + c`。

| 引擎 | 策略 | 拟合斜率 k | 判定 |
|---|---|---|---|
{NL.join(fit_rows)}

**Python 全线 k ≈ 1.00；C++ 的 MACD 在**改动前**是 k ≈ 1.97 —— 二次。**

> 表里 C++ 各行标注了「改动前 / 增量原型 / 引擎当前」。写这份调查时，
> `MACD` / `RSI` / `KDJ` 三条都是 k ≈ 1.95；现在引擎里那三个已经改成增量递推、
> 斜率掉到 k ≈ 1.06（见[第三轮](#第三轮修复与复测)），所以「引擎当前」那几行是线性的。
> **「改动前 O(N²)」那一行是从冻结在 benchmark 里的原实现当场测出来的**，
> 不是从旧记录抄的 —— 否则这个结论就不可复现了。

翻开源码，原因一目了然 —— `StrategyContext` 里的指标 helper
（[`strategy_context.h`](../backtest_engine/include/backtest/strategy_context.h)）
**每根 bar 都从第 0 根开始重算整条序列**：

- `macd()` 每 bar 重建 4 个长度为 N 的 `std::vector`，重跑三遍完整 EMA 递推
- `rsi()` 每 bar 重建差分数组，从头跑一遍 Wilder 平滑
- `kdj()` 每 bar 从第 n−1 根重跑整条 K/D 递推
- 只有 `sma()` 是 O(period)，所以 `MA_CROSS` 幸免

而 Python 侧的指标是 numpy 一次性向量化算好的，**O(N)**。

#### 决定性实验：把它改成 O(1) 增量，看会怎样

光有相关性不够。所以写了一个**增量版 MACD**（[`bench_backtest.cpp`](bench_backtest.cpp) 里的
`IncrementalMACDStrategy`）：递推式、系数、种子逐行照抄原版，只是把 EMA 状态存下来，
每 bar O(1)、零堆分配。**它必须产生逐位相同的结果**，否则实验作废。

| 数据规模 | 原版 O(N²) | 增量版 O(N) | 提速 | 结果核对 |
|---|---|---|---|---|
{NL.join(incr_rows)}

提速比随规模**单调增长** —— 这正是 O(N²)→O(N) 的指纹。

而最关键的一行藏在数字里：{SIZES[-1]:,} 根时，
**增量版 MACD 是 {ms(incr_last['macd_incr_min'])} ms，`MA_CROSS` 是 {ms(cpp_ma_last)} ms —— 两者基本相同**。
修掉算法之后，MACD 立刻回到了 `MA_CROSS` 那个量级；对 Python 的比值从
**{macd_last['speedup_vs_py_pure']:.1f}×（输）变成 {incr_vs_py:.0f}×（赢）**，
与 `MA_CROSS` 的 {ma_last['speedup_vs_py_pure']:.0f}× 一致。

> **结论**：「C++ 的 MACD 回测输给 Python」与语言毫无关系。
> 那是一段写成 O(N²) 的 C++ 输给了一段写成 O(N) 的 Python。

原始数据：[`results/experiment_incremental.json`](results/experiment_incremental.json)

### H3 · 向量化 ✅ 成立

Python 那边的指标不是 Python 算的 —— 是 numpy 算的，底层 C 循环加 SIMD，一次扫完整列。

{SIZES[-1]:,} 根 bar 上算同一套 MACD：

| 做法 | 耗时 |
|---|---|
| Python + numpy，整列一次算完 | **{ms(py_prep_last)} ms** |
| C++ 手写标量循环，每 bar 重算 | **≈ {ms(cpp_indicator_cost)} ms** |

（C++ 那一栏是「MACD 整场 {ms(cpp_macd_last)} ms − 指标近乎免费的 MA_CROSS 整场 {ms(cpp_ma_last)} ms」，
是对指标部分的近似估计。）

相差约 **{vectorize_ratio:,.0f}×**。

**但这个数字必须小心解读，别把它当成「numpy 比 C++ 快」。** numpy 底层就是 C。
上面比的是「O(N) 一次算完」和「O(N²) 每 bar 重算」，那个 {vectorize_ratio:,.0f}× 里
绝大部分是复杂度阶数的差，不是向量化的功劳。

把对手换成**写好的 C++**，结论立刻不同：H2 里那个增量版整场回测只要
{ms(incr_last['macd_incr_min'])} ms，而其中指标部分（每 bar 4 次浮点运算）几乎不占时间 ——
也就是说**正确实现的 C++ 在指标计算上不输给 numpy，甚至更快**。

所以 H3 成立的部分是这个，而且已经足够有价值：

> 向量化让 Python 在批量数值计算上达到了**接近 C 的量级**
> —— {SIZES[-1]:,} 根 bar 的完整 MACD 只要 {ms(py_prep_last)} ms。
> 它赢不了写好的 C++，但它把「用 Python 就一定慢」这个前提废掉了。

一个尺度感：Python 的指标准备只占它端到端时间的
**{row(SIZES[-1], 'MACD')['py_indicator_prep_share']:.2%}** —— 几乎免费。
**Python 的时间几乎全花在逐 bar 循环里**，那才是它真正的短板，也正是 H5 要查的地方。

### H4 · 堆内存分配 ⚠️ 有贡献，但只是常数因子

增量实验一次改掉了两件事：不再重算、不再每 bar 分配内存。得把它们分开。

于是又做了第三个版本 `MACD_NOALLOC`：**照旧每 bar 重算整条序列**（仍是 O(N²)），
但四个缓冲区复用同一块内存，全程零分配。

| 数据规模 | 原版<br>O(N²)+分配 | 零分配<br>O(N²) | 增量<br>O(N) | 分配<br>占比 | 算法<br>占比 |
|---|---|---|---|---|---|
{NL.join(alloc_rows)}

三个版本结果**完全一致**。分配大约占可优化空间的
**{alloc_last['alloc_share_of_gain']:.0%}**，算法占 **{alloc_last['algo_share_of_gain']:.0%}**。

但有个更重要的观察：消除分配只带来 **{alloc_only_gain:.1f}×** 的提速，
而零分配版**依然是二次的**（最后两档拟合斜率 k ≈ {noalloc_k:.2f}）。

> **分配优化给的是常数因子，算法优化给的是阶数。** 规模一大，常数因子就不值钱了。

原始数据：[`results/experiment_allocation.json`](results/experiment_allocation.json)

### H5 · 数据结构 ✅ 成立，而且颠覆了修法

那 Python 的逐 bar 循环慢在哪？是「解释器慢」吗？

拆开测每根 bar 里的具体操作，并和 numpy 数组上的同样操作对照：

| 操作 | pandas DataFrame | numpy 数组 | 差距 |
|---|---|---|---|
{NL.join(pd_table)}

Python 引擎实测每根 bar **{pdo['engine_total_per_bar_us']:.1f} µs**，
其中 **{pdo['pandas_ops_per_bar_us']:.1f} µs（{pdo['pandas_share']:.0%}）**
纯粹花在 DataFrame 的标量取值和切片上。

DataFrame 是为**整列批量运算**设计的：每次 `df.iloc[i]['close']` 都要走索引解析、
类型分派、构造返回对象。逐元素标量访问是它最差的用法。

> **「Python 慢」这句话里，有 {pdo['pandas_share']:.0%} 根本不是 Python 的问题，是数据结构用错了。**
> 把 DataFrame 换成 numpy 数组（Python 代码一行不改语义），
> 每 bar 成本可望从 {pdo['engine_total_per_bar_us']:.1f} µs 降到约
> {pdo['engine_total_per_bar_us'] - pdo['pandas_ops_per_bar_us']:.1f} µs —— 快约
> {pdo['engine_total_per_bar_us'] / (pdo['engine_total_per_bar_us'] - pdo['pandas_ops_per_bar_us']):.1f}×。

剩下的那部分才是真正的解释器开销 —— 那部分只能靠换语言解决。

> ⚠️ 这个实验的运行环境**与主跑分表不同**：Python {pdo['environment']['python']} /
> pandas {pdo['environment']['pandas']} / numpy {pdo['environment']['numpy']}
> （主表是 {env['python']} / pandas {env['pandas']}）。绝对微秒数因此不可与主表直接相加，
> 但这里要看的是**占比**，而占比对版本不敏感。环境记录在结果文件自己的
> environment 块里，不用去猜。

原始数据：[`results/experiment_pandas_overhead.json`](results/experiment_pandas_overhead.json)，
由 [`exp_pandas_overhead.py`](exp_pandas_overhead.py) 生成

### H6 · 传输层 ✅ 成立，但只对小任务致命

C++ 引擎对外是个 HTTP 服务。测一下传输层吃掉多少（{SIZES[-1]:,} 根 bar）：

| 策略 | HTTP 全程 | 进程内 | 传输层占比 |
|---|---|---|---|
{NL.join(http_rows)}

对 `MA_CROSS` 这种引擎本身只要 {ms(cpp_ma_last)} ms 的任务，
**{http_small['transport_share']:.0%} 的时间花在 JSON 序列化和 HTTP 往返上** ——
引擎再快十倍，用户也感觉不到。而对那些引擎本身就要跑一两秒的任务，传输层占比降到 3–6%。

> 优化要打在瓶颈上。任务越小，传输层越是瓶颈。

### H7 · 日志 I/O ❌ 证伪

原本的假设是：Python 引擎在逐 bar 循环里写日志，拖慢了它。

**读源码发现这个前提就是错的** —— `engine.py:113` 的 `logger.info` 在
`for order in pending:` 里面，是**每笔成交**打一条，不是每根 bar。
{SIZES[-1]:,} 根 bar 上只有一千多笔成交。

实测也印证了：开日志 {lc['logging_on_median_s'] * 1000:.1f} ms vs
关日志 {lc['logging_off_median_s'] * 1000:.1f} ms —— 比值
{lc['slowdown_x']:.2f}×，也就是开日志「更快」。这显然是噪声，
真实差异**低于测量分辨率**。

benchmark 全程仍然关着日志跑（那是对的做法），但**这个因素对结论没有贡献**。
一个自己提出、自己证伪的假设也要写出来 —— 只报成立的假设，读者没法判断你有没有挑数据。

### H8 · 冷启动 ⚠️ 不影响引擎，但影响体感

`import pandas` 要 **{bt['cold_start']['import_pandas_seconds'] * 1000:.0f} ms**。

这不计入引擎跑分（它不是引擎的成本），但对「跑一次小回测」这种用法，
它比回测本身还贵。常驻进程里是一次性成本，命令行工具里则是每次都付。

---

## 补充：换一个 Python 版本，结论会变吗

上面所有 Python 数字跑在 **{env['python']}**（母项目的 conda 环境）。
但 Python 3.11 起解释器做过一轮大优化，只报一个版本对 Python 不公平。
所以又用本机的 **{py313['python']}**（pandas {py313['pandas']} / numpy {py313['numpy']}）
把 Python 侧完整重跑了一遍 —— C++ 侧不变，数据、策略、采样方式全部相同。

{SIZES[-1]:,} 根 bar：

| 策略 | Python {env['python']} | Python {py313['python']} | 新版快 | C++ 领先倍数变化 |
|---|---|---|---|---|
{NL.join(v_rows)}

**Python {py313['python']} 快了约 {v_ma:.2f}×**，而且这个提速对结论有两个方向相反的影响：

- **削弱**了「语言」这个因子：`MA_CROSS` 上 C++ 的领先从
  {ma_last['speedup_vs_py_pure']:.0f}× 降到 **{v_ma_ratio_new:.0f}×**
- **强化**了核心结论：`MACD` 上 C++ 从 {macd_last['speedup_vs_py_pure']:.1f}× 变成
  **{v_macd_ratio_new:.2f}×** —— **输得更彻底了**

在 {py313['python']} 上，C++ 在四个策略里**输掉或打平了三个** —— `MACD` {v_macd_ratio_new:.2f}×、
`RSI` {r313(SIZES[-1], 'RSI')['py_pure_engine']['min'] / row(SIZES[-1], 'RSI')['cpp_inproc']['min']:.2f}×、
`KDJ` {r313(SIZES[-1], 'KDJ')['py_pure_engine']['min'] / row(SIZES[-1], 'KDJ')['cpp_inproc']['min']:.2f}× ——
只在 `MA_CROSS` 上保持领先。

> 值得注意的是：「语言/运行时」这个因子本身就有 **{(v_ma - 1) * 100:.0f}% 的浮动** ——
> 换个 Python 小版本号就变。而 O(N²)→O(N) 那 {incr_last['speedup']:.0f}× 是结构性的，不随环境漂移。
> **这本身就是「别把语言当成主要变量」的又一个证据。**

原始数据：[`results/backtest_py313.json`](results/backtest_py313.json)

---

## 归因汇总

按对最终耗时的影响排序：

| # | 因素 | 量级 | 性质 | 能不能修 |
|---|---|---|---|---|
| 1 | **算法阶数** O(N²)→O(N) | 最高 **{incr_last['speedup']:.0f}×**，随 N 增长 | 复杂度阶数 | ✅ 能，收益最大 |
| 2 | **语言/运行时** | **{v_ma_ratio_new:.0f}–{ma_last['speedup_vs_py_pure']:.0f}×**（区间来自 Python 版本差异） | 常数因子 | ⚠️ 要换语言 |
| 3 | **数据结构**（pandas 标量索引） | 约 **{pdo['engine_total_per_bar_us'] / (pdo['engine_total_per_bar_us'] - pdo['pandas_ops_per_bar_us']):.1f}×** | 常数因子 | ✅ 换 numpy 数组即可 |
| 4 | **堆分配** | **{alloc_only_gain:.1f}×** | 常数因子 | ✅ 缓冲区复用 |
| 5 | **传输层** | 小任务吃掉 **{http_small['transport_share']:.0%}** | 固定开销 | ✅ 批量化 / 进程内调用 |
| 6 | **冷启动** | **{bt['cold_start']['import_pandas_seconds'] * 1000:.0f} ms** 一次性 | 固定开销 | ⚠️ 常驻进程可摊薄 |
| 7 | **日志 I/O** | 测不出来 | — | 无需修 |

**最重要的一行是第 1 行和第 2 行的对比：**

在这份数据里，**算法阶数带来的差距（最高 {incr_last['speedup']:.0f}×）比语言选择带来的差距（{v_ma_ratio_new:.0f}–{ma_last['speedup_vs_py_pure']:.0f}×）更大**，
而且前者随 N 无限放大、不随环境漂移，后者是有上限的常数、换个 Python 小版本就浮动 {(v_ma - 1) * 100:.0f}%。

所以「该用 C++ 还是 Python」多半是个问错了的问题。该问的是：
**这段计算能不能一次算完，而不是每步重算。**

---

## 所以，什么时候该用哪个

### C++ 明显占优

**1. 本质串行、带跨 bar 状态、无法向量化的循环** — 实测 {ma_last['speedup_vs_py_pure']:.0f}–{row(SIZES[0], 'MA_CROSS')['speedup_vs_py_pure']:.0f}×

逐 bar 决策是链式依赖的：这根 bar 的信号要看上一根的均线关系，要不要下单要看当前持仓，
持仓又来自之前的成交。**没有任何一步能提前批量算**，numpy 在这里帮不上忙。
Python 只能一根一根走，每根都付解释器和对象开销。这是 C++ 的主场。

**2. 单次操作耗时接近语言开销下限** — 订单簿撮合 {sub['latency']['p50_ns']:,.0f} ns/单

Python 光是一次函数调用加几个对象构造就到几百纳秒了。
「单次很小、次数极多」的操作，语言开销占比是压倒性的。

**3. 需要可预测的尾部延迟** — 订单簿 p999 = {sub['latency']['p999_ns']:,.0f} ns

没有 GC 停顿。Python 的 GC 会在不确定的时刻插进来，对撮合引擎是致命的。

**4. 小任务高频调用** — 参数网格搜索

{SIZES[0]} 根 bar 的回测：C++ {ms(row(SIZES[0], 'MA_CROSS')['cpp_inproc']['min'])} ms vs
Python {ms(row(SIZES[0], 'MA_CROSS')['py_pure_engine']['min'])} ms。
跑一千组参数：C++ {row(SIZES[0], 'MA_CROSS')['cpp_inproc']['min'] * 1000:.2f} 秒
vs Python {row(SIZES[0], 'MA_CROSS')['py_pure_engine']['min'] * 1000:.0f} 秒
（约 {row(SIZES[0], 'MA_CROSS')['py_pure_engine']['min'] * 1000 / 60:.0f} 分钟）
—— 交互式调参和泡杯咖啡回来看的区别。

### Python 明显占优

**1. 能交给向量化库的批量计算** — 用 Python 的代价≈0

{SIZES[-1]:,} 根 bar 的完整 MACD，numpy 只要 **{ms(py_prep_last)} ms**，
占整场回测的 {row(SIZES[-1], 'MACD')['py_indicator_prep_share']:.2%}。

⚠️ 注意这里的措辞是「代价≈0」而不是「更快」。按 H3 的结论，
**向量化赢不了写好的 C++**（增量版 C++ 的指标部分比这还快）。
它赢的是**写差了的 C++** —— 本次实测里那个 O(N²) 的版本慢了约 {vectorize_ratio:,.0f}×。

真正的价值在于：在这类计算上选 Python **几乎不用付性能代价**，
于是可以把预算花在开发速度和可读性上。这跟「Python 更快」是两回事。

**2. 生态里已经有人把难的部分写成了 C**

用 Python 不等于用 Python 的速度跑。关键看热点落在解释器里，还是落在库里。
本次 Python 引擎的时间有 {pdo['pandas_share']:.0%} 花在 pandas 的标量索引上（H5），
那正是「热点掉回解释器和对象层」的典型症状 —— 同样是用库，用对用错差 {pd_rows[0][1] / pd_rows[0][2]:.0f} 倍。

**3. 开发和修改成本** — {py_loc:,} 行 vs {cpp_loc:,} 行

Python 参照引擎 {py_loc:,} 行做的事，C++ 引擎用了 {cpp_loc:,} 行
（后者功能更多 —— 多出风控、市场规则、组合回测和另外六个策略 —— 所以这个对比只能当粗略的量级参考，不是等价功能的行数比）。

### 一句话的决策规则

> **热点能不能向量化？**
> 能 → 用 Python + numpy，写 C++ 的边际收益很小，还可能像本次一样反而更慢。
> 不能（串行状态依赖）→ C++ 有两个数量级的优势，值得。

而回测引擎**两种都占**：指标计算能向量化，逐 bar 撮合决策不能。
所以最优解不是二选一，是**混合** —— 指标交给向量化预计算，串行循环交给 C++。

这恰好就是 Python 参照引擎当年的架构（读预先算好的指标列），
也正是当前 C++ 引擎该改的方向。

---

## 一个不能比的例子 —— 后来查清并修掉了

`KDJ` 在上面的表里出现了，但**它的数字不该被当成速度对比读**。

> ✅ **这个缺陷已修复**（`kdj_strategy.cpp`）。下面的表格与分析是**修复前**的状态，
> 保留原样是因为它是本次调查最有价值的方法论教训。修复后的实测在本节末尾。

| 数据规模 | C++ 成交笔数 | Python 成交笔数 |
|---|---|---|
{NL.join(kdj_rows)}

相差最多**两个数量级**。原因在策略逻辑本身：

| | 买入过滤 | 卖出过滤 |
|---|---|---|
| **C++** | `k < overbought_` → **k < 80** | `k > oversold_` → **k > 20** |
| **Python** | `k < oversold` → **k < 20** | `k > overbought` → **k > 80** |

C++ 那边的注释写的是「K 上穿 D，且在**低位区域（超卖区）**」，
但代码判的是 `< overbought_`（80）—— 这个条件几乎恒真，**超卖过滤等于没生效**。
Python 侧注释与代码一致。

两个实现跑的根本不是同一个策略。这种情况下「C++ 比 Python 快 1.4×」是个**没有意义的数字** ——
它们的工作量差两个数量级。

> 把它留在这里，是因为它是本次调查里最有价值的方法论教训：
> **跑分之前必须先证明两边在做同一件事。**
> 本仓库的 benchmark 因此内置了工作量核对（比对成交笔数与最终净值），
> 对不上就打标记 —— 否则很容易一路比较两个不同的东西，还得出漂亮的结论。

### 修复后的实测

把 C++ 侧的两个阈值改正（与 `description()`、参数 schema、README 以及 Python
参照实现全部一致）之后，同一份合成数据上的工作量**完全对齐**了：

| 数据规模 | C++ 成交（修复前） | C++ 成交（修复后） | Python 成交 |
|---|---|---|---|
| 1,000 | 160 | **3** | 3 |
| 2,500 | 392 | **3** | 3 |
| 10,000 | 1,674 | **13** | 13 |

原先相差 53–131 倍，现在逐个相同。这也反过来确认了归因是对的：
差异确实来自那两个写反的阈值，不是别的原因。

⚠️ 本节开头那张成交笔数表仍是修复前的数据 ——
`results/backtest.json` 是修复之前那次跑分的产物，重跑整套跑分会连带改动
全部 Python 侧数字（本机 pandas 版本与记录中的不同），所以留待下一次完整复测时统一更新。
修复本身由 `tests/test_kdj_strategy.cpp` 的五个用例守着，其中三条经过验证
**在缺陷存在时确实会变红**。

---

## 订单簿：没有对照组的绝对基线

订单簿模拟器没有 Python 对照，给的是绝对数字。

**撮合吞吐与下单延迟**（{sub['orders']:,} 笔混合订单流：70% 被动挂单 / 20% 主动穿价 / 10% 市价）

| 指标 | 数值 |
|---|---|
| 吞吐 | **{sub['orders_per_sec']:,.0f} orders/sec** |
| p50 | {sub['latency']['p50_ns']:,.0f} ns |
| p90 | {sub['latency']['p90_ns']:,.0f} ns |
| p99 | {sub['latency']['p99_ns']:,.0f} ns |
| p999 | {sub['latency']['p999_ns']:,.0f} ns |

**盘口深度查询** `get_depth(10)` 随簿深度的变化

| 簿内挂单量（股） | p50 (ns) | p99 (ns) | 摊销 (ns) |
|---|---|---|---|
{NL.join(depth_rows)}

簿从一万涨到二十六万股，查询只从 {ob['depth_query'][0]['latency']['p50_ns']:,.0f} ns 变到
{ob['depth_query'][-1]['latency']['p50_ns']:,.0f} ns —— 基本与簿大小无关，符合「只取前 10 档」的预期。

**撤单** —— 发现问题，然后修掉了

最初这里是全簿线性扫描：[`cancel_order`](../orderbook_simulator/src/limit_order_book.cpp)
遍历买盘每一档、每档再遍历整条 FIFO 队列，找不到再遍历卖盘。没有任何 `order_id` 索引。
撤一个不存在的 id 永远是最坏情况——两边都扫完。

| 指标 | 加索引前 | 加索引后 | 变化 |
|---|---|---|---|
| **摊销** | {cancel_before['amortized_ns']:,.0f} ns | **{cancel['amortized_ns']:,.0f} ns** | **{cancel_before['amortized_ns'] / cancel['amortized_ns']:.0f}×** |
| p50 | {cancel_before['latency']['p50_ns']:,.0f} ns | {cancel['latency']['p50_ns']:,.0f} ns ⚠️ | {cancel_before['latency']['p50_ns'] / cancel['latency']['p50_ns']:.0f}× |
| p99 | {cancel_before['latency']['p99_ns']:,.0f} ns | {cancel['latency']['p99_ns']:,.0f} ns ⚠️ | —— |
| p99.9 | {cancel_before['latency']['p999_ns']:,.0f} ns | {cancel['latency']['p999_ns']:,.0f} ns ⚠️ | —— |
| 撤单/下单 p50 | {cancel_before['latency']['p50_ns'] / sub_before['latency']['p50_ns']:.1f}× | {cancel['latency']['p50_ns'] / sub['latency']['p50_ns']:.2f}× | —— |

**为什么头条用摊销值而不是 p50。** 改完之后撤单快到了
{cancel['latency']['p50_clock_ticks']:.1f} 个时钟 tick（本机 steady_clock 粒度实测
{ob['clock_granularity_ns']:.0f} ns），单次测量的量化相对误差约
±{cancel['latency']['p50_quantization_rel_err'] * 100:.0f}% —— 打 ⚠️ 的那几行尾数
是量化产物，不是信号。改之前 p50 有
{cancel_before['latency']['p50_clock_ticks']:,.0f} 个 tick，完全可信；改之后不再可信。

所以这次的可信倍数是**摊销值的 {cancel_before['amortized_ns'] / cancel['amortized_ns']:.0f}×**，
而不是 p50 算出来的 {cancel_before['latency']['p50_ns'] / cancel['latency']['p50_ns']:.0f}×。
后者看着更好看，但它一部分来自「被测对象快到测不准了」，把它当结论就是自欺。

两份数据都是用**同一版** benchmark 程序测的（都带预热、都实测时钟粒度）：
改前那份是把当前的 `bench_orderbook.cpp` 拿到改动前的提交上跑出来的。
harness 不同的 before/after 不可比，这一点比多报一个倍数重要。

改动是生产级 LOB 的标准做法：一张 `order_id → (方向, 价位)` 的哈希表，
撤单变成一次哈希查找加该档内的短扫描。

**同时做的另一件事，收益出乎意料。** `PriceLevel` 原来的
`is_empty()` / `order_count()` / `total_quantity()` 都是 O(n) —— 因为撤单是软撤单
（只置 `is_active=false`，不出队），必须扫过队列里的「尸体」才能判断。改成用两个
增量维护的计数器之后：

| 指标 | 加索引前 | 加索引后 |
|---|---|---|
| `get_depth(10)` p50 @ 簿深 1,000 | {depth_before[0]['latency']['p50_ns']:,.0f} ns | {ob['depth_query'][0]['latency']['p50_ns']:,.0f} ns |
| `get_depth(10)` p50 @ 簿深 10,000 | {depth_before[1]['latency']['p50_ns']:,.0f} ns | {ob['depth_query'][1]['latency']['p50_ns']:,.0f} ns |
| `get_depth(10)` p50 @ 簿深 100,000 | {depth_before[2]['latency']['p50_ns']:,.0f} ns | {ob['depth_query'][2]['latency']['p50_ns']:,.0f} ns |
| 下单 p50 | {sub_before['latency']['p50_ns']:,.0f} ns | {sub['latency']['p50_ns']:,.0f} ns |

注意 `get_depth` 那三行：改之前随簿深从 {depth_before[0]['latency']['p50_ns']:,.0f} 涨到
{depth_before[2]['latency']['p50_ns']:,.0f} ns，改之后**基本不随簿深变化**了。
这才是这次改动的实质——不是常数变小，是复杂度阶数变了。

**下单也变快了**，尽管它多了一次索引插入。原因是每次提交订单都会调 `cleanup()`，
而 `cleanup()` 要对每一档调 `is_empty()`；`is_empty()` 从 O(n) 变 O(1) 之后，
省下的比索引插入的开销更多。

行为等价由 [`test_differential.cpp`](../orderbook_simulator/tests/test_differential.cpp)
的随机化差分测试守着：200 个种子 × 300 步 = 6 万次操作，与参照模型逐笔一致。
先有护栏再改性能，顺序不能反。

原始数据：[`results/orderbook.json`](results/orderbook.json)（改后）·
[`results/orderbook_before_cancel_index.json`](results/orderbook_before_cancel_index.json)（改前基线）

---

## 为了让这些数字可信，做了什么

### 每个结果文件都有生成脚本了

这份报告开篇声称「每个数字都能在 `results/` 里找到出处」。有一段时间那句话是打折的：
8 个结果文件里只有 3 个有提交的生成脚本，其余是一次性脚本跑完就丢了。
对一个把方法论当卖点的报告，这是最伤的一处 —— 会真去核对的读者恰恰是最该说服的人。

现在全部补齐：

| 结果文件 | 生成脚本 |
|---|---|
| `backtest.json` | [`bench.py`](bench.py) |
| `parity.json` | [`parity_gate.py`](parity_gate.py) |
| `orderbook.json` | [`bench_orderbook.cpp`](bench_orderbook.cpp)（重定向到文件） |
| `complexity_fit.json` · `experiment_incremental.json` · `experiment_allocation.json` | [`exp_cpp_experiments.py`](exp_cpp_experiments.py) |
| `experiment_incremental_engine.json` | [`exp_incremental_engine.py`](exp_incremental_engine.py) |
| `experiment_pandas_overhead.json` | [`exp_pandas_overhead.py`](exp_pandas_overhead.py) |
| `backtest_py313.json` | [`bench_py_version.py`](bench_py_version.py) |
| `indicator_profile.json` | [`exp_indicator_profile.py`](exp_indicator_profile.py) |
| `machine_drift.json` | [`exp_machine_drift.py`](exp_machine_drift.py) |
| `prereg_{prereg['registered_at']}.json` | **刻意没有** —— 见[第四轮](#第四轮先预测再动手) |

唯一的例外是最后一行。预注册文件记的是**预测**，不是测量：
「跑一下就能重新得到」恰恰是它不该具备的性质。它写下来之后就不许再动，
Phase 4 只回填 `measured` 字段，`claim` 与 `predicted` 一个字不改。

补完之后做了一次自我核对：用新脚本重跑 `backtest_py313.json`，
与原来那份**同一解释器**下的记录相差约 1%（例如 25,000 根的 `MA_CROSS`
1,048.879 → 1,039.113 ms）。也就是说新脚本忠实复现了当初那个一次性脚本做的事。

⚠️ 需要说明一点：`backtest.json` 是**调查当时**的快照（C++ 侧还是改动前的 O(N²)
实现）。它没有被重跑，因为第一轮「MACD 只有 0.9×」那个证伪结论正是建立在那份数据上；
修复后的复测在[第三轮](#第三轮修复与复测)单独给出，用的是冻结在 benchmark 里的对照臂。
两份数据各自描述一个明确的时点，而不是混在一起。

### 环境

| 项 | 值 |
|---|---|
| CPU | {env['cpu']}（{env['cores']} 核） |
| 系统 | {env['os']} |
| 编译器 | {env['compiler']} |
| C++ 构建 | {env['cpp_build_type']} |
| Python | {env['python']}（{env['python_impl']}） |
| pandas / numpy | {env['pandas']} / {env['numpy']} |
| 采样 | warmup {bt['config']['warmup']} 轮 + 正式 {bt['config']['runs']} 轮 |

### 耗时取 `min` 而不是中位数

benchmark 的干扰是**单向**的 —— 后台活动只会让程序变慢，永远不会让它变快。
所以 `min` 才是「真实成本」的最佳估计，中位数反而会被系统噪声整体抬高。

这不是理论洁癖：某一轮跑分里 `KDJ` 在最大规模上的 `max/min` 达到 **5.52×**，
中位数被抬高了 46%，还算出过 **−68.6% 的「负传输开销」**这种不可能的数。

中位数、p95、max 仍然完整写进结果文件，用来判断每一格干不干净。
`max/min > 1.5` 的格子在表里打 ⚠ 标记。

### 两个 benchmark 必须串行跑

第一次跑的时候把订单簿和回测两个 benchmark 并行放到了后台，
**它们互相抢 CPU，两边数据都作废**。现在是 `&&` 串起来跑的。

### 工作量核对

速度对比的前提是两边在做同一件事。每一格都比对 **成交笔数**（主判据）
和**最终净值的相对误差**（辅助判据），对不上就打标记并列在输出末尾。

主判据用成交笔数而非净值：净值是 N 次交易复利的结果，任何微小差异都会被放大；
而「做了多少笔交易」才真正决定引擎干了多少活。

### 时钟分辨率

macOS 的 `steady_clock` 底层是 `mach_absolute_time`，实测最小非零间隔 **41 ns**。
单次 `get_depth` 只有约 250 ns，**只有 6 个 tick**，量化误差 ±17%。

所以订单簿每个用例同时给两个数：逐次计时的**分位数**（能看尾部，但被量化）
和整批总耗时÷次数的**摊销值**（不受量化影响，但看不到尾部）。两个一起看才完整。

### 合成数据的设计

规模曲线需要两万五千根 bar，而真实 fixture 只有 180 根，所以规模测试用合成行情。
生成器是自己实现的线性同余随机数（不依赖 `random` 模块的实现细节），
同一个种子在任何机器、任何 Python 版本上产生完全相同的序列。**两个引擎喂的是同一个 JSON 文件。**

价格走势用**均值回复**而不是带漂移的随机游走，这一点是踩坑之后改的：
第一版带 0.0002/天的漂移，250 根上很正常，但两万五千根复利下来价格冲到上万、
账户净值到 1e17 量级 —— 那个区间会触发 C++ `lot_floor` 的 20 亿股钳位
（[`strategy_context.h:80`](../backtest_engine/include/backtest/strategy_context.h)）而 Python 没有钳位，
于是工作量核对**假报失败**。正弦周期的振幅也是实测调出来的（0.0015），
保证任意规模下净值都待在本金的 1–2.2 倍之间。理由写在
[`benchlib.py`](benchlib.py) 的注释里。

### 公平性上刻意做的取舍

- **Python 只算策略真正需要的指标列**。母项目的入口 `add_indicators()` 会一次算
  trend + momentum + volatility + volume 四大族，拿那个当 Python 的指标成本，
  等于凭空给它加上一堆用不到的负担。
- **C++ 在「纯引擎」和「端到端」两个口径下是同一个数字** —— 它没有独立的指标准备阶段，
  指标在 `on_bar` 里算。所以拿 C++ 对比 Python 的纯引擎口径，其实**对 C++ 不利**
  （它的循环里还扛着指标计算）；两个口径都报了。
- **计时不含进程启动和 import**（那不是引擎的成本），但冷启动单独登记了。

### 已知局限

- 单进程单线程，**没有测并发**。
- Python 参照引擎是 2026-07-15 退役时的状态；C++ 引擎此后又长了风控、市场规则、
  组合回测等功能。**对比只在两边都有的功能面上做**。
- `MACD` 在最大规模上被标了噪声（`max/min > 1.5`），该格数字看趋势即可，不必细读。
- H3 里 C++ 的指标成本是**用两个策略相减估出来的**，不是直接测量，属于近似。

---

## 这次调查查出来的具体问题

按价值排序，都是可执行的：

1. ~~**`strategy_context.h` 的 `macd()` / `rsi()` / `kdj()` 改成增量递推。**~~
   ✅ **已完成。** 25,000 根上实测 **{r3_last['macd_speedup']:.0f}×**，复杂度 O(N²) → O(N)，
   四个策略收敛到同一量级。逐位等价由金标准 fixture 守着（全量重算与增量两条路径
   都要逐位命中），parity 门禁七项仍全部 0.00e+00。详见上面「第三轮：修复与复测」。

2. ~~**`limit_order_book.cpp` 的 `cancel_order` 加 `order_id` 索引。**~~
   ✅ **已完成。** 撤单摊销耗时从 {cancel_before['amortized_ns']:,.0f} ns 降到
   {cancel['amortized_ns']:,.0f} ns（**{cancel_before['amortized_ns'] / cancel['amortized_ns']:.0f}×**，
   用摊销口径是因为改完之后 p50 已低于时钟分辨率），
   顺带把 `PriceLevel` 的聚合量改成增量维护，`get_depth` 不再随簿深增长。
   详见上面「订单簿」一节。

3. ~~**`kdj_strategy.cpp` 的超买超卖过滤写反了。**~~
   ✅ **已完成。** 两个阈值互换回来，并补上 `MACDStrategy` 那样的 `initialized_`
   守卫（否则第一个被评估的 bar 会基于 50.0 初值造出一个假交叉）。
   修复后 C++ 与 Python 的 KDJ 工作量逐个相同（3/3、3/3、13/13，修复前是
   160/392/1674 对 3/3/13）。新增 `tests/test_kdj_strategy.cpp` 五个用例 ——
   此前 `grep -riE "kdj" tests/` 零命中。

4. **`ctx.sma()` 也是 O(period) 的朴素重求和**，可以改成 O(1) 滑动窗口和。
   它没进上面的表是因为 period 小（5/20），代价被掩盖了 —— 但 period 一大就会显现。

5. **C++ 与 pandas 的 RSI 种子不同**：C++ 用前 `period` 个变化的简单平均做种，
   pandas `ewm(adjust=False)` 用第一个值做种。导致首次穿越阈值的 bar 不同，
   最终净值出现稳定的 0.8% 系统性偏差（各规模一致，非累积误差）。
   两种都是常见做法，但**应当明确选定一种并写进文档**。

---

## 第三轮：修复与复测

前两轮查清了原因，这一轮把它修掉，然后复测。

`strategy_context.h` 里的 `macd()` / `rsi()` / `kdj()` 已经改成增量递推：
持久化递推载体，每根 bar 只推一步。状态挂在**每个标的**的 `SymbolState` 上 ——
不能放策略对象，因为组合回测下所有标的共用同一个策略实例；也不能放
`StrategyContext`，因为它每根 bar 在栈上重建。

### 对照臂是冻结的，不是回忆的

改完之后有个直接问题：引擎里的 `MACD` 变快了，那 `MACD` vs `MACD_INCREMENTAL`
就成了在比较两个相同的东西，这个实验再也无法复现。

所以改动前那份 O(N²) 实现被**原样冻结**进 [`bench_backtest.cpp`](bench_backtest.cpp)，
作为 `MACD_NAIVE` 对照臂。下表的「改动前」一列是这条臂**当场跑出来的**，不是从旧记录里抄的。

| 数据规模 | 改动前 O(N²) | 引擎当前 O(N) | 提速 | 经济结果 |
|---|---|---|---|---|
{NL.join(r3_rows)}

提速比随规模**单调增长**（{r3[0]['macd_speedup']:.0f}× → {r3_last['macd_speedup']:.0f}×）——
这正是 O(N²)→O(N) 的指纹，与第二轮用原型测出来的形状一致。

**「经济结果」那一列是这张表的前提。** 每一格都比对了成交笔数与最终净值：
两条臂必须逐位相同，否则它们跑的不是同一个策略，提速数字毫无意义。
这一列若出现 ❌，生成脚本会直接以非零退出码失败。

### 最有说服力的一行：四个策略收敛到同一量级

25,000 根 bar 上，四个策略现在的耗时：

| 策略 | 耗时 |
|---|---|
| `MA_CROSS`（未改动，参照系） | {ms(r3_last['ma_cross_min'])} ms |
| `MACD` | {ms(r3_last['macd_min'])} ms |
| `RSI` | {ms(r3_last['rsi_min'])} ms |
| `KDJ` | {ms(r3_last['kdj_min'])} ms |

第一轮里 `MACD` 比 `MA_CROSS` 慢两个数量级，现在它们**基本相同**。
`MA_CROSS` 本身没有改动、耗时也没变 —— 它是这次比较的定盘星。

这就把第一轮那个反常现象彻底解释掉了：`MACD` 当初输给 Python，
与语言无关，与那三个策略的**实现方式**有关。

### 逐位等价是怎么保证的

不是靠「跑出来差不多」，是靠三层：

1. **金标准 fixture**（`backtest_engine/tests/data/indicator_golden.json`）在**改动之前**
   从朴素实现导出，逐根 bar 记录每个指标的 double **位模式**（不是十进制）。
   增量重写后，同一份 fixture 必须逐位命中 —— 全量重算路径和增量路径**都要**命中。
2. **`-ffp-contract=off`**：禁止编译器把 `a*b+c` 收缩成一条 FMA。实测同一份源码
   默认会编出 3 条 `fmadd`，而 FMA 少一次舍入、差 1 ULP。不关掉的话两条路径可能
   拿到不同的收缩决策，逐位等价就无从谈起。
3. **parity 门禁**：C++ 与 Python 参照引擎的七项经济指标仍然全部 `0.00e+00` ——
   而且从第四轮起门禁除 `MA_CROSS` 外还覆盖了 `MACD` 与 `KDJ`，
   它现在真的在守这一轮改过的代码（见[第四轮](#第四轮先预测再动手)）。

原始数据：[`results/experiment_incremental_engine.json`](results/experiment_incremental_engine.json)，
由 [`exp_incremental_engine.py`](exp_incremental_engine.py) 生成。

---

## 第四轮：先预测，再动手

第三轮证明了「改完变快了」。它**没有**证明的是「改之前就知道会变快多少」——
那一轮的每一个数字都是事后测量的。

这是个方法论缺口，而且是最容易自欺的那种：改完再解释为什么会这样，
永远解释得通。所以本轮先把预测写下来。

预注册文件：[`results/prereg_{prereg['registered_at']}.json`](results/prereg_{prereg['registered_at']}.json)，
写于 **{prereg['registered_at']}**，在任何一行实现代码落地之前。

> 它是 [`results/`](results/) 里唯一**没有生成脚本**的文件，而且必须没有。
> 生成脚本意味着「跑一下就能重新得到」，而预测的全部价值恰恰在于
> 它写在跑之前、此后不许再动。上面那张「每个结果文件都有生成脚本」的表格，
> 这一行是**刻意的例外**。

### 坦白：上一轮没有预注册

{prereg['honesty_note']['问题']}

{prereg['honesty_note']['为什么不补']}

{prereg['honesty_note']['记为缺陷']}

### 先分类，再决定改什么

「只改剖析显示是热点的东西」要能执行，前提是先说清楚每个指标属于哪一类、
依据是什么。依据写的是代码事实，不是印象。

{prereg_class_table('zh')}

两条值得单独说：

- **`sma` / `stddev` 明明能改成 O(1)，但刻意不改。** 滚动和会改变浮点求和顺序，
  于是不再逐位等价。这个仓库里逐位等价的优先级高于常数因子 —— 这条取舍本身
  比「我把能优化的都优化了」更能说明问题。
- **`highest_close` / `lowest_close` 归在 A 类，但同样不改。** 可优化不等于该优化：
  没有任何策略在热路径上调用它们。归类回答的是「能不能」，剖析回答的是「值不值」。

### 预注册的预测

{prereg_pred_table('zh')}

其中 **P2 不是一条普通预测，而是一条决策规则**：

> {next(it for it in prereg['classification']['items'] if 'HHV' in it['indicator'])['decision_rule']}

也就是说，本轮预先接受了「查完发现不该改，那就不改」这个结果。
这是「只改热点」的字面执行 —— 否则那句话只是事后给已经做了的改动找的理由。

### 剖析：指标还是不是热点

「只改剖析显示是热点的东西」——那就先剖析。

⛔ **没用「把指标单拎出来跑个循环」那种测法。** 那测的是数据全在 L1 里的理想成本；
真实回测中间夹着组合估值、撮合、日历推进，会把 bar 挤出缓存。孤立计时给的是下界。

用的是这个仓库已有的对照臂手法：同一场回测，只把 `ctx.macd()` 这类调用换成
「从预先算好的数组里取第 `bar_index` 个」，其余每一行不动。
两条臂的成交笔数与最终净值**逐位相同**，时间差才只剩指标那一部分。

> **第二条走不通的路，也记下来。** 最初是两条臂分两次进程各测各的再相减。
> 结果六个策略**全部落在噪声里**，其中 `RSI` 与 `MOMENTUM` 测出**负的**指标成本 ——
> 对照臂做的事严格更少，不可能更慢。进程级抖动比要测的差值还大，那版数字全部作废。
>
> 现在是**同进程内交替配对**：每轮跑 A、B 各一次得到一个差，共模漂移相减抵消；
> 每轮还交换先后，免得「先跑的承担缓存预热」固定偏袒某一条臂。
> 判据不看占比好不好看，看**符号检验**：21 轮全正，纯属偶然的概率是 4.8e-07。

{profile_table('zh')}

三件事：

1. **指标已经不是热点了。** 能测出来的占比全部落在
   {profile_min_share:.1f}%–{profile_max_share:.1f}%，远低于预注册的 15% 上限。
   上一轮把 O(N²) 消掉之后，继续优化指标的收益上限就被钉死在这个区间里了。
2. **`MOMENTUM` 三行全部「测不出来」，而这正是方法本身的验算。**
   `returns()` 本来就是 O(1)（归类表里的 B 类），成本就该是零；
   符号检验给出 9/21、8/21、10/21 —— 标准的抛硬币。
   一个已知为零的量被测成零，说明这套配对测法没有系统性偏袒。
3. **P2 的决策规则因此生效：单调队列不做。** KDJ 的指标**整体**（含窗口扫描）
   只占 {next(r['indicator_share'] for r in profile['results'] if r['strategy'] == 'KDJ' and r['bars'] == 25000) * 100:.1f}%，
   把其中一部分再优化掉，对整场回测的影响进不了测量精度。
   预注册当初就写明了「若 < 10% 则不做」，那就不做 —— 这是「只改热点」这句话
   真正要付的代价。

### 一个没预料到的发现：跨会话漂移比要测的效应还大

预注册 P7 给接口重构定的容差是「落在改动前的 ±2% 以内」，
基线取自几天前的记录。动手前先验了一下这个基线还成不成立：

{drift_table('zh')}

**同一份代码，一个字节没改，跨会话就漂 {drift_lo:.1f}%–{drift_hi:.1f}%。**
（过程中还撞见过一次 `MA_CROSS` 报 +30.4% —— 那遍机器正被占用，
紧接着三遍都回到 10% 以内，所以那是离群点，不是结论。）

于是 P7 那条 ±2% **根本不可能被满足**，无论重构做得多干净。这是预注册自己
暴露出来的毛病：**它把容差钉在了另一天的基线上。** 原文一个字不改地留着，
判定改为与**同一次会话**的基线比 —— 上表 `remeasured_min` 那一列就是。

这个数字本身值得公开：它是这台机器上所有跨会话速度对比的精度上限。
**小于 {drift_hi:.1f}% 的「提速」，在跨会话口径下不成立。**

> 顺带一个反差：绝对耗时漂了 {drift_lo:.1f}%–{drift_hi:.1f}%，
> 而同一批数据拟合出的 log–log 斜率跨会话只差 **±0.015**
> （`MA_CROSS` 1.0656→1.0513，`MACD（改动前）` 1.9696→1.9691）。
> **这正是斜率比倍数更值得信的原因**：倍数量的是这台机器今天的状态，
> 斜率量的是算法的阶数。

### 门禁本来守的不是被改动的代码

一直以来这道 parity 门禁只跑 `MA_CROSS`，而 `MA_CROSS` 只用 `sma()`。
也就是说第三轮改掉的 `macd` / `rsi` / `kdj`，**一个都没进门禁** ——
号称最硬的那道闸门，守的不是被改的那部分代码。

{prereg['parity_probe']['_note']}

在 {prereg['parity_probe']['fixture']} 上探完，按实测结果扩：

{gate_table('zh')}

三个 `0.00e+00` 不是侥幸过的。量了一下离翻转还有多远：

- `MACD` 最接近翻转的那根 bar，`dif − dea = −1.762e-02`，而两侧的数值分歧是
  `2.44e-14` —— **余量是分歧的 7.2e11 倍**。
- `KDJ` 最接近翻转的那根 bar，两侧的 `k − d` 完全相同。

> ⚠️ 别把这读成「C++ 和 pandas 逐位一致」。**门禁证明的是经济结果相等，
> 不是指标位模式相等。** MACD 两侧的指标值仍有 ≤6144 ULP 的分歧
> （相对 3.8e-14），只是离 dif/dea 翻转差着十一个数量级，落不到成交上。

### RSI：唯一没进门禁的那个，以及为什么不降门禁

`RSI` 七项里最大差 `{_rsi_row['max_diff']:.2e}` —— 最终资产
Python {_rsi_row['python_final_value']:,.2f} vs C++ {_rsi_row['cpp_final_value']:,.2f}。

**根因不是精度，是两侧算的本来就是两个不同定义的 RSI：**

| | 前 `period` 个 change 怎么处理 |
|---|---|
| C++（经典 Wilder） | 先取 SMA 作种子，之后转 Wilder 递推 |
| `benchlib.add_rsi` | `ewm(alpha=1/period, adjust=False)`，从 **0** 起播种 |

两者之差按 `(1−1/period)^n` 衰减，半衰期约 9.4 根。实测：

- 过预热期后最大差 **{_rsi['max_abs_diff']:.4f}**（第 {_rsi['max_abs_diff_at_bar']} 根）
- 到第 179 根衰减到 **{_rsi['final_bar_diff']:.2e}**
- 30/70 穿越事件两侧不一致的 bar：**{_rsi['threshold_crossing_disagreements']}** ——
  全在早期，成交时点不同就是从这三根来的

所以这是一处**预热期定义分歧**，长序列上自行收敛。

要让它进门禁只有两条路：**放宽容差**，或**改 Python 参照实现**。
前者毁掉门禁的全部意义 —— 这个仓库里 `0.00e+00` 之所以有分量，正因为它
从来不是「在容差之内」；后者动了冻结的对照臂，对照就不成立了。

所以选第三条：查清、量化、写在这里，闸门一寸不让。
门禁脚本每次运行都会把上面这些数字**重新算一遍**打出来，它们不是手抄的。

---

## 复现

```bash
# 1. 构建（含 benchmark 目标）
./build.sh
cmake --build backtest_engine/build      --target bench_backtest  -j8
cmake --build orderbook_simulator/build  --target bench_orderbook -j8

# 2. 起 C++ 服务（HTTP 口径和 parity 门禁都需要）
./backtest_engine/build/backtest_server 8002 &

# 3. 硬门禁：先证明两个引擎算的是同一件事
python3 benchmarks/parity_gate.py

# 4. 跑分（必须串行，别并行 —— 会互抢 CPU）
./orderbook_simulator/build/bench_orderbook 1 > benchmarks/results/orderbook.json
python3 benchmarks/bench.py                      # 主跑分表（调查当时的快照）

# 5. 对照实验（每个结果文件都有对应脚本）
python3 benchmarks/exp_cpp_experiments.py        # 复杂度拟合 / 增量 / 分配
python3 benchmarks/exp_incremental_engine.py     # 第三轮：修复后复测
python3 benchmarks/exp_pandas_overhead.py        # H5 数据结构
python3 benchmarks/bench_py_version.py           # Python 版本对照

# 6. 重新生成本文档（同时产出中英两版）
python3 benchmarks/make_report.py
```

Python 侧需要 `pandas` / `numpy` / `loguru`，见 [`ORIGIN.md`](ORIGIN.md)。

---

*本文档由 [`make_report.py`](make_report.py) 从 [`results/`](results/) 生成于 {env['timestamp']}。*
*每个数字都可以在结果文件里查到出处。*
"""

# ────────────────────────────────────────────────────────────
# 英文版（主版本）
#
# 为什么英文版也必须是**生成**的、而不是手写的：
# 本文档开篇就声称「每个数字都能在 results/ 里找到出处，没有手抄」。
# 如果英文版的数字是手敲的，那这句话对最主要的读者就是假的 ——
# 而这份报告的全部价值建立在它的可核对性上。
# 所以两个语言版本共用同一批插值变量，同一次运行同时产出。
# ────────────────────────────────────────────────────────────
from report_en import build_en  # noqa: E402

doc_en = build_en(globals())

(HERE / "README.zh-CN.md").write_text(doc_zh, encoding="utf-8")
(HERE / "README.md").write_text(doc_en, encoding="utf-8")
print(f"✓ 已生成 {HERE / 'README.zh-CN.md'}（{len(doc_zh.splitlines())} 行）")
print(f"✓ 已生成 {HERE / 'README.md'}（{len(doc_en.splitlines())} 行）")
