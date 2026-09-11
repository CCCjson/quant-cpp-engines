# quant-cpp-engines

> Two low-level trading engines written from scratch in C++17: an **event-driven backtest engine**
> and a **limit order book matching simulator**. Each ships its own REST API server.
> No dependency installation — CMake `FetchContent` pulls everything at build time.

*[中文版 / Chinese version](README.zh-CN.md)*

[![CI](https://github.com/CCCjson/quant-cpp-engines/actions/workflows/ci.yml/badge.svg)](https://github.com/CCCjson/quant-cpp-engines/actions/workflows/ci.yml)

---

## What's actually interesting here

If you only read one thing, read the **[performance investigation](benchmarks/README.md)**.
It does not conclude "C++ is faster."

The project's own design doc claimed *"the C++ per-bar loop is much faster than Python."*
That sentence had never been measured. So it got measured — against a **real Python backtest
engine recovered from a parent project's git history** (retired once the C++ version matured,
so it is a genuine control, not something written to lose).

The starting hypothesis was falsified by the first round of data:

| Strategy (25,000 bars) | C++ vs Python |
|---|---|
| `MA_CROSS` | **242×** |
| `MACD` | **0.9×** ← C++ lost |

Same language, same engine, same data. Change the strategy and the conclusion reverses.

Root cause turned out to be **algorithmic complexity order**, not language: the engine's
`macd()` / `rsi()` / `kdj()` recompute the entire series from bar 0 on every bar
(log-log fit slope k ≈ 1.95, i.e. O(N²)), while the Python side's indicators are computed
once by numpy in O(N).

> **The gap from complexity order is larger than the gap from language choice** — the former
> grows without bound in N, the latter is a bounded constant.

Seven hypotheses, controlled experiments, and the known limits of each claim:
**[`benchmarks/README.md`](benchmarks/README.md)**

### Three other things a reviewer might care about

**A randomized differential test found a real fairness bug in the matching engine.**
Random order flow is replayed against both the real book and a deliberately slow, obviously
correct reference model, comparing fills *and* full book state at every step. It found this
(seed 12648430, auto-shrunk from 300 steps to 3): two orders at the same nominal price
`100.07`, computed via two algebraically equivalent formulas, land on **two different
`std::map<double, ...>` keys** — so the later order fills first and the earlier one is
skipped. That violates time priority, which this project's own docs promise. Details and
the acceptance tests: [`orderbook_simulator/README.md`](orderbook_simulator/README.md).

**A bit-exact parity gate runs in CI.** Before any performance number is quoted, the C++ and
Python engines must agree on seven economic metrics to `0.00e+00` — not "within tolerance,"
bit-identical. It holds on both macOS/clang and Linux/gcc.

**Measurement honesty is enforced in the data, not just the prose.** After the order book got
fast enough that some operations take 2 clock ticks, the benchmark now measures the machine's
actual clock granularity and marks which of its own percentiles are quantization-limited.
One published speedup was revised down from 502× to 312× for exactly this reason.

---

## Layout

| Directory | Contents |
|---|---|
| [`backtest_engine/`](backtest_engine/) | Event-driven backtest engine. 10 strategies, portfolio backtesting on a shared cash pool, risk/commission/slippage modeling |
| [`orderbook_simulator/`](orderbook_simulator/) | Limit order book + matching engine. Four order types, market impact estimation, isolated sessions |
| [`benchmarks/`](benchmarks/) | **The performance investigation**: C++ vs a real Python reference engine, seven hypotheses tested one by one, with controls |
| [`examples/`](examples/) | Pure-stdlib Python demo client — exercises both engines in one command |
| [`docs/`](docs/) | Original design docs (superseded by the per-project READMEs) |

---

## Quick start

Requires **CMake ≥ 3.16** and a **C++17** compiler (clang / gcc / MSVC).
The first build fetches three header-only dependencies from GitHub, so stay online.

```bash
# Build both engines and run every unit test
./build.sh

# Same, under AddressSanitizer + UndefinedBehaviorSanitizer
./build.sh asan
```

Start the servers:

```bash
./backtest_engine/build/backtest_server 8002 &
./orderbook_simulator/build/orderbook_server 8001 &
```

Run the demo (Python standard library only, nothing to install):

```bash
python3 examples/python_client.py
```

Reproduce the parity gate — the hard precondition for every performance claim:

```bash
pip install -r benchmarks/requirements.txt
./backtest_engine/build/backtest_server 8002 &
python3 benchmarks/parity_gate.py     # exit 0 = all seven metrics at 0.00e+00
```

---

## What's verified, and how

| | |
|---|---|
| Unit tests | 134 GoogleTest cases (92 backtest + 42 order book) |
| Compiler warnings | `-Wall -Wextra -Werror` on both engines, zero warnings |
| Floating point | `-ffp-contract=off` — the FP operation sequence is pinned by the source, not by the compiler's FMA decisions |
| Sanitizers | Full suite green under ASan + UBSan, `-fno-sanitize-recover=all` |
| CI | ubuntu×{gcc,clang} × {Release,Debug}, macos×clang, sanitizers, and the parity gate |
| Cross-engine parity | Seven economic metrics bit-identical to the Python reference, enforced per push |
| Indicator values | Golden fixture pins every indicator on every bar as a **bit pattern**, not a decimal |
| Matching logic | Randomized differential test, 200 seeds × 300 steps = 60,000 operations vs a reference model |

Three tests are deliberately `DISABLED_`: they are the acceptance criteria for the
floating-point price defect described above, and will be enabled by the fix.
A permanently red CI is the same as no CI, so known defects get a disabled test plus
documentation rather than a red badge.

```bash
./orderbook_simulator/build/orderbook_tests --gtest_also_run_disabled_tests \
    --gtest_filter='PriceIntegrity*'
```

---

## Dependencies

All three are fetched at build time by CMake `FetchContent`. **Nothing to `apt install` or
`brew install`.**

| Dependency | Purpose | Form |
|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) `v3.11.3` | JSON serialization | header-only |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | REST API server | header-only |
| [GoogleTest](https://github.com/google/googletest) `v1.14.0` | Unit tests | build-time |

---

## Design positions

These two engines are production components extracted from a live quant system, not toy demos.
A few choices were deliberate:

**1. Look-ahead bias is a hard constraint.**
A signal generated on bar N is always filled at the **open of bar N+1**. Filling today's
signal at today's close is never permitted. China A-share T+1 is modeled for real on
`Position.available` — shares bought today cannot be sold today; they unfreeze at the next open.

**2. Silent failure is rejected.**
Bad input returns 400 rather than a plausible-looking 200:
- Not a single bar inside the requested date window → `400`, instead of silently running on everything;
- Duplicate leg symbols in a portfolio backtest → `400`, instead of the later one overwriting
  the earlier and quietly collapsing "a 10-symbol portfolio" into 3;
- Orders dropped on the final bar because there is no "next open" are returned explicitly as
  `dropped_last_bar_orders`;
- How many days each symbol actually had data is returned as `bar_coverage` —
  **a missing bar is not a zero**.

**3. Portfolio backtests share one cash pool.**
N symbols compete for a single balance, rather than each getting a full copy of the capital,
running independently, and having their returns averaged. Without cash competition, a weighted
portfolio strategy produces numbers that have nothing to do with its weights. When several buy
orders compete for the same cash on the same day, they are scaled down proportionally by
notional, and how much was trimmed comes back as `cash_contention`.

**4. Matching follows real exchange semantics.**
Price priority, then time priority (FIFO within a level). MARKET / LIMIT / IOC / FOK each
implement their real semantics — FOK genuinely pre-checks for all-or-nothing rather than
matching and rolling back.

**5. Known defects are written down, with tests.**
The order book README carries a "Known issues" section, currently including the
floating-point price key defect. Publishing a defect you found and have not yet fixed is
better than publishing a claim that is not true.

---

## License

MIT
