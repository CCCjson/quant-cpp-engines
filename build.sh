#!/usr/bin/env bash
#
# build.sh — 构建两个引擎并跑全部单元测试
#
# 用法：
#   ./build.sh            # Release 构建 + 跑测试
#   ./build.sh Debug      # Debug 构建 + 跑测试
#   ./build.sh asan       # Debug + ASan/UBSan 构建 + 跑测试
#   ./build.sh tsan       # Debug + ThreadSanitizer 构建 + 跑测试（查数据竞争）
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu || echo 4 )"

MODE="${1:-Release}"
SANITIZE=OFF
TSAN=OFF
# asan 模式用独立的构建目录 build-asan/，不与 Release 的 build/ 混用。
# 混用会踩两个坑：一是每次切换都要全量重编，二是很容易拿着上一次遗留的
# 非 sanitizer 产物以为自己在跑 sanitizer。
BUILD_DIR="build"
if [ "${MODE}" = "asan" ] || [ "${MODE}" = "ASan" ] || [ "${MODE}" = "ASAN" ]; then
    BUILD_TYPE="Debug"
    SANITIZE=ON
    BUILD_DIR="build-asan"
elif [ "${MODE}" = "tsan" ] || [ "${MODE}" = "TSan" ] || [ "${MODE}" = "TSAN" ]; then
    # TSan 与 ASan 互斥，所以用独立的构建目录和独立的开关
    BUILD_TYPE="Debug"
    TSAN=ON
    BUILD_DIR="build-tsan"
else
    BUILD_TYPE="${MODE}"
fi

build_one() {
    local name="$1"
    echo ""
    echo "════════════════════════════════════════════════════════"
    echo "  构建 ${name}  (${BUILD_TYPE}, asan=${SANITIZE}, tsan=${TSAN}, -j${JOBS})"
    echo "════════════════════════════════════════════════════════"
    cmake -S "${ROOT}/${name}" -B "${ROOT}/${name}/${BUILD_DIR}" \
          -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
          -DENABLE_SANITIZERS="${SANITIZE}" \
          -DENABLE_TSAN="${TSAN}"
    cmake --build "${ROOT}/${name}/${BUILD_DIR}" -j"${JOBS}"
}

build_one backtest_engine
build_one orderbook_simulator

echo ""
echo "════════════════════════════════════════════════════════"
echo "  单元测试"
echo "════════════════════════════════════════════════════════"
"${ROOT}/backtest_engine/${BUILD_DIR}/backtest_tests"
"${ROOT}/orderbook_simulator/${BUILD_DIR}/orderbook_tests"

echo ""
echo "✅ 全部通过。可执行文件："
echo "   ${ROOT}/backtest_engine/${BUILD_DIR}/backtest_server        (默认 :8002)"
echo "   ${ROOT}/orderbook_simulator/${BUILD_DIR}/orderbook_server   (默认 :8001)"
