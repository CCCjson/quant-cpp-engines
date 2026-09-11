# ============================================================
# 订单簿模拟器 — CMake 构建配置
# ============================================================
#
# CMake 是 C++ 项目的"构建系统生成器"。
# 你不直接写 Makefile，而是写 CMakeLists.txt，
# CMake 帮你生成对应平台的 Makefile / Xcode / VS 工程。
#
# 构建命令:
#   mkdir build && cd build
#   cmake ..          ← 生成 Makefile
#   make -j4          ← 编译（-j4 表示用 4 个线程并行编译）
#
# ============================================================

# 最低 CMake 版本要求
cmake_minimum_required(VERSION 3.16)

# 项目名称和语言
project(orderbook_simulator LANGUAGES CXX)

# ── C++ 标准 ──
# C++17 是目前主流的 C++ 标准版本
# 它支持 std::optional、结构化绑定、if constexpr 等现代特性
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)    # 必须支持 C++17，不降级

# ── 编译选项 ──
# 用 INTERFACE 库承载选项，再逐个 target 链接。
#
# 这里原来是目录级的 add_compile_options(-Wall -Wextra)，问题是它会波及下面
# FetchContent 拉进来的 googletest / httplib / json——那是别人的代码，加上
# -Werror 之后会直接炸掉构建。改成 target 级，选项只作用于本项目自己的代码。
add_library(orderbook_flags INTERFACE)
target_compile_options(orderbook_flags INTERFACE
    -Wall -Wextra

    # -ffp-contract=off：禁止把 a*b+c 收缩成一条 FMA 指令（少一次舍入，差 1 ULP）。
    # 订单簿这边暂时没有递推式，但两个引擎保持同一套浮点约定，
    # 以免以后把某段计算从一边搬到另一边时结果对不上。
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
    target_compile_options(orderbook_flags INTERFACE -Werror)
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
    target_compile_options(orderbook_flags INTERFACE
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer   # 让 ASan 的栈回溯可读
        -g
    )
    # sanitizer 必须同时作用于链接期，否则运行时库不会被链进来
    target_link_options(orderbook_flags INTERFACE
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
    target_compile_options(orderbook_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(orderbook_flags INTERFACE -fsanitize=thread)
    message(STATUS "ThreadSanitizer: ON")
endif()

# ============================================================
# FetchContent — 自动下载第三方库
# ============================================================
#
# FetchContent 是 CMake 内置的包管理器
# 它会在第一次 cmake 时从 GitHub 下载代码，之后用缓存
# 不需要手动安装任何依赖！

include(FetchContent)

# ── nlohmann/json ──
# C++ 最流行的 JSON 库，用法简单：
#   nlohmann::json j = {{"name", "test"}, {"price", 100.0}};
#   std::string s = j.dump();  // 序列化为字符串
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE       # 浅克隆，只拉最新版本，节省时间
)

# ── cpp-httplib ──
# 轻量级 HTTP 服务器/客户端库，只有一个头文件
# 用它可以在 C++ 里快速搭建 REST API
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
)

# ── GoogleTest ──
# Google 出品的 C++ 单元测试框架
# 写法类似: TEST(TestSuite, TestName) { EXPECT_EQ(1+1, 2); }
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

# 下载并配置所有依赖（第一次会比较慢）
FetchContent_MakeAvailable(json httplib googletest)

# ============================================================
# 主程序
# ============================================================

# 收集所有源文件
# GLOB_RECURSE 会递归搜索 src/ 目录下所有 .cpp 文件
file(GLOB_RECURSE SOURCES "src/*.cpp")

# 创建可执行文件
add_executable(orderbook_server ${SOURCES})

# 设置头文件搜索路径
# 这样代码里可以写 #include "orderbook/types.h" 而不用写完整路径
target_include_directories(orderbook_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 链接第三方库
target_link_libraries(orderbook_server PRIVATE
    orderbook_flags
    nlohmann_json::nlohmann_json    # JSON 库
    httplib::httplib                 # HTTP 服务器库
)

# ============================================================
# Benchmark 程序
# ============================================================
# 进程内直接打 MatchingEngine / LimitOrderBook，不经 HTTP。
# 源码放在仓库的 benchmarks/ 目录（与本子项目平级）。

set(BENCH_OB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/bench_orderbook.cpp)
if(EXISTS ${BENCH_OB_SRC})
    # 除 main.cpp 外的全部源文件（bench 自带 main）
    file(GLOB_RECURSE OB_LIB_SOURCES "src/*.cpp")
    list(FILTER OB_LIB_SOURCES EXCLUDE REGEX "main\\.cpp$")

    add_executable(bench_orderbook ${BENCH_OB_SRC} ${OB_LIB_SOURCES})
    target_include_directories(bench_orderbook PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(bench_orderbook PRIVATE
        BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
    target_link_libraries(bench_orderbook PRIVATE
        orderbook_flags
        nlohmann_json::nlohmann_json
        httplib::httplib
    )
endif()

# ============================================================
# 单元测试
# ============================================================

# enable_testing() 告诉 CMake "这个项目有测试"
enable_testing()

# 收集测试文件
file(GLOB TEST_SOURCES "tests/*.cpp")

# 收集除了 main.cpp 之外的源文件（测试不需要 main 函数）
file(GLOB_RECURSE LIB_SOURCES "src/*.cpp")
list(FILTER LIB_SOURCES EXCLUDE REGEX "main\\.cpp$")

# 创建测试可执行文件
add_executable(orderbook_tests ${TEST_SOURCES} ${LIB_SOURCES})

target_include_directories(orderbook_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(orderbook_tests PRIVATE
    orderbook_flags
    nlohmann_json::nlohmann_json
    httplib::httplib
    GTest::gtest_main               # GoogleTest + 自带 main 函数
)

# 注册测试，这样可以用 ctest 命令运行
include(GoogleTest)
gtest_discover_tests(orderbook_tests)
