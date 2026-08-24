#!/usr/bin/env bash
#
# build.sh — 构建两个引擎并跑全部单元测试
#
# 用法：
#   ./build.sh            # Release 构建 + 跑测试
#   ./build.sh Debug      # Debug 构建 + 跑测试
#
set -euo pipefail

BUILD_TYPE="${1:-Release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu || echo 4 )"

build_one() {
    local name="$1"
    echo ""
    echo "════════════════════════════════════════════════════════"
    echo "  构建 ${name}  (${BUILD_TYPE}, -j${JOBS})"
    echo "════════════════════════════════════════════════════════"
    cmake -S "${ROOT}/${name}" -B "${ROOT}/${name}/build" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    cmake --build "${ROOT}/${name}/build" -j"${JOBS}"
}

build_one backtest_engine
build_one orderbook_simulator

echo ""
echo "════════════════════════════════════════════════════════"
echo "  单元测试"
echo "════════════════════════════════════════════════════════"
"${ROOT}/backtest_engine/build/backtest_tests"
"${ROOT}/orderbook_simulator/build/orderbook_tests"

echo ""
echo "✅ 全部通过。可执行文件："
echo "   ${ROOT}/backtest_engine/build/backtest_server        (默认 :8002)"
echo "   ${ROOT}/orderbook_simulator/build/orderbook_server   (默认 :8001)"
