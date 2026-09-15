# ============================================================
# CMakeLists.txt — C++ 回测系统的构建配置
# ============================================================
#
# 这个文件告诉 CMake 如何编译我们的项目。
# CMake 是一个"构建系统的生成器"——它不直接编译代码，
# 而是生成 Makefile（或 Xcode/VS 项目），再用 make 来编译。
#
# 使用方式：
#   mkdir build && cd build
#   cmake ..
#   make -j$(nproc)
# ============================================================

cmake_minimum_required(VERSION 3.14)       # 要求 CMake 3.14+（FetchContent 需要）
project(backtest_cpp VERSION 1.0.0 LANGUAGES CXX)  # 项目名 + 版本 + 语言

# ── C++ 标准 ──
set(CMAKE_CXX_STANDARD 17)                # 使用 C++17 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON)       # 必须支持 C++17，否则报错

# ── 编译选项 ──
# 用 INTERFACE 库承载选项，再逐个 target 链接，而不是用目录级的
# add_compile_options()。原因：下面 FetchContent 拉进来的 googletest /
# httplib / json 是别人的代码，目录级选项会波及它们，-Werror 会在
# 第三方代码上炸掉构建。
add_library(backtest_flags INTERFACE)
target_compile_options(backtest_flags INTERFACE
    -Wall -Wextra

    # -ffp-contract=off：禁止把 a*b+c 收缩成一条 FMA 指令。
    #
    # 这不是洁癖，是本项目的正确性前提。指标（macd/rsi/kdj）的递推式长这样：
    #     alpha * close + (1.0 - alpha) * prev
    # clang 默认 -ffp-contract=on，实测这一行会被合成一条 fmadd。FMA 只做
    # 一次舍入，分开算做两次，结果差 1 ULP。
    #
    # 只要「朴素全量重算版」和「增量递推版」在不同的内联上下文里拿到了不同的
    # 收缩决策，两者就不再逐位相同——而「逐位相同」正是 benchmarks/ 里那套
    # 等价性论证赖以成立的东西。关掉收缩，让浮点运算序列由源码唯一确定。
    -ffp-contract=off
)

# ── 警告是否视为错误 ──
# 默认 ON：本项目自己的代码不允许留警告。
# 留出开关是因为不同编译器的 -Wall/-Wextra 集合并不一致——GCC 会报一些
# clang 不报的东西（-Wmaybe-uninitialized 在 -O3 下尤其容易误报）。
# 换编译器或换版本时如果撞上误报，可以用
#     -DWARNINGS_AS_ERRORS=OFF
# 临时放行，而不必把 -Wall -Wextra 一起关掉——那才是真正的损失。
option(WARNINGS_AS_ERRORS "把编译警告视为错误" ON)
if(WARNINGS_AS_ERRORS)
    target_compile_options(backtest_flags INTERFACE -Werror)
endif()

# ── Sanitizer（可选）──
# 用法：cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
# 或者直接 ./build.sh asan
#
# ASan 抓内存错误（越界、use-after-free、泄漏），UBSan 抓未定义行为
# （有符号溢出、错误的 static_cast、空指针解引用等）。
#
# 这个项目里 UBSan 尤其有用：引擎里有几处 double → int 的转换，注释里
# 记着「raw static_cast 一个 8e9 的 double 是 UB，会得到 INT_MIN」这类
# 教训（见 risk_manager.cpp 的 _qty_from_headroom）。这类问题只有工具
# 抓得住，靠读代码是漏的。
#
# -fno-sanitize-recover=all：出问题直接 abort，而不是打一行日志继续跑。
# CI 里必须这样，否则 UBSan 的发现会被淹在输出里、退出码还是 0。
option(ENABLE_SANITIZERS "开启 AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    target_compile_options(backtest_flags INTERFACE
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer   # 让 ASan 的栈回溯可读
        -g
    )
    # sanitizer 必须同时作用于链接期，否则运行时库不会被链进来
    target_link_options(backtest_flags INTERFACE
        -fsanitize=address,undefined
    )
    message(STATUS "Sanitizers: ON (ASan + UBSan, 不可恢复)")
endif()

# ── ThreadSanitizer（可选，与 ASan 互斥）──
# 用法：./build.sh tsan
#
# TSan 抓数据竞争。它和 ASan **不能同时开**（两者都要接管内存布局），
# 所以是独立的一个选项，而不是塞进 ENABLE_SANITIZERS。
#
# 这个项目需要它：REST 服务器默认开 8+ 个工作线程，而并发安全是靠
# 「每 session 一把锁 + 读写锁护表 + 原子 ID」保证的 —— 这类保证只有
# TSan 能验证，靠读代码是看不住的。
option(ENABLE_TSAN "开启 ThreadSanitizer（与 ENABLE_SANITIZERS 互斥）" OFF)
if(ENABLE_TSAN)
    if(ENABLE_SANITIZERS)
        message(FATAL_ERROR "ENABLE_TSAN 与 ENABLE_SANITIZERS 不能同时开：TSan 和 ASan 互斥")
    endif()
    target_compile_options(backtest_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(backtest_flags INTERFACE -fsanitize=thread)
    message(STATUS "ThreadSanitizer: ON")
endif()

# ── FetchContent：自动下载第三方库 ──
# 好处：不需要手动安装依赖，cmake 会自动从 GitHub 下载
include(FetchContent)

# nlohmann/json — JSON 解析库（header-only，只有头文件，不需要编译）
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE       # 只下载最新版本，不下载全部历史
)

# cpp-httplib — HTTP 服务器库（header-only）
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
)

# GoogleTest — 单元测试框架
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

# 让 CMake 去下载并准备这些库
FetchContent_MakeAvailable(json httplib googletest)

# ── 头文件搜索路径 ──
# 告诉编译器去哪里找我们的 #include 文件
# include/ 目录放公共头文件，src/ 目录放策略实现的头文件
include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# ── 源文件列表 ──
# 列出所有需要编译的 .cpp 文件（不包括 main.cpp 和测试文件）
set(LIB_SOURCES
    src/portfolio.cpp
    src/engine.cpp
    src/metrics.cpp
    src/data_loader.cpp
    src/server.cpp
    src/risk_manager.cpp
    src/strategies/ma_cross_strategy.cpp
    src/strategies/momentum_strategy.cpp
    src/strategies/macd_strategy.cpp
    src/strategies/rsi_strategy.cpp
    src/strategies/kdj_strategy.cpp
    src/strategies/bollinger_strategy.cpp
    src/strategies/combo_strategy.cpp
    src/strategies/pairs_strategy.cpp
    src/strategies/external_signal_strategy.cpp
    src/strategies/portfolio_signal_strategy.cpp
)

# ── 主程序：backtest_server ──
add_executable(backtest_server
    src/main.cpp           # 入口 main 函数
    ${LIB_SOURCES}         # 所有库代码
)

# 链接第三方库到主程序
target_link_libraries(backtest_server PRIVATE
    backtest_flags
    nlohmann_json::nlohmann_json    # JSON 库
    httplib::httplib                # HTTP 服务器库
)

# ── Benchmark 程序 ──
# 进程内计时用，绕开 HTTP 层直接构造 BacktestEngine。
# 源码放在仓库的 benchmarks/ 目录（与本子项目平级），复用这里的 LIB_SOURCES 和依赖。
set(BENCH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/bench_backtest.cpp)
if(EXISTS ${BENCH_SRC})
    add_executable(bench_backtest ${BENCH_SRC} ${LIB_SOURCES})
    target_link_libraries(bench_backtest PRIVATE
        backtest_flags
        nlohmann_json::nlohmann_json
        httplib::httplib
    )
endif()

# ── 指标金标准生成器 ──
# 一次性工具：把当前实现算出的每根 bar 的指标值导出成 fixture，产物提交进仓库。
# 单独一个 target 而不是给测试加 --regen 开关，是刻意的：
# 测试**不应该**有能力覆写自己的期望值，否则「测试通过」随时可以靠重新生成
# 期望值来达成，金标准就失去意义了。
add_executable(indicator_golden_gen tests/gen_indicator_golden.cpp ${LIB_SOURCES})
target_compile_definitions(indicator_golden_gen PRIVATE
    GOLDEN_BARS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/data/cpp_bars.json"
)
target_link_libraries(indicator_golden_gen PRIVATE
    backtest_flags
    nlohmann_json::nlohmann_json
    httplib::httplib
)

# ── 测试程序 ──
enable_testing()   # 启用 CMake 的测试功能

# 所有测试文件
set(TEST_SOURCES
    tests/test_portfolio.cpp
    tests/test_metrics.cpp
    tests/test_engine.cpp
    tests/test_strategies.cpp
    tests/test_external_signal.cpp
    tests/test_portfolio_engine.cpp
    tests/test_pairs_strategy.cpp
    tests/test_indicator_golden.cpp
    tests/test_kdj_strategy.cpp
    tests/test_strategy_reset.cpp
)

add_executable(backtest_tests
    ${TEST_SOURCES}
    ${LIB_SOURCES}
)

# 金标准 fixture 与真实日线的路径由构建系统注入，测试不依赖运行时 cwd
target_compile_definitions(backtest_tests PRIVATE
    GOLDEN_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/indicator_golden.json"
    GOLDEN_BARS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/data/cpp_bars.json"
)

target_link_libraries(backtest_tests PRIVATE
    backtest_flags
    nlohmann_json::nlohmann_json
    httplib::httplib
    GTest::gtest_main              # GoogleTest（自带 main 函数）
)

# 注册测试到 CTest（可以用 ctest 命令运行）
include(GoogleTest)
gtest_discover_tests(backtest_tests)
