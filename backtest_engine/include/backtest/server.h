/*
 * server.h — REST API 服务器
 *
 * 提供 HTTP 接口，让 Python FastAPI 可以调用 C++ 回测引擎。
 *
 * 端点（与 src/server.cpp 的 setup_routes() 保持一致）：
 * GET  /api/strategies             — 策略清单及参数 schema（返回前 8 个）
 * POST /api/backtest/run           — 单标的策略回测
 * POST /api/backtest/run_signals   — 外部信号回放
 * POST /api/backtest/run_portfolio — 组合回测（N 个标的共享一份现金）
 *
 * 使用 cpp-httplib 库提供 HTTP 服务。
 *
 * 注：这段注释原先列的是 `POST /api/backtest/data`（从来不存在），
 * 同时漏掉了实际存在的 run_signals 和 run_portfolio。
 * 已按实现更正 —— 一份骗人的接口清单比没有清单更糟。
 */

#pragma once

#include <string>

namespace backtest {

class Server {
public:
    explicit Server(int port = 8002);

    /*
     * start — 启动服务器
     * 这个方法会阻塞（一直运行直到调用 stop()）
     */
    void start();

    /*
     * stop — 停止服务器
     */
    void stop();

private:
    int port_;

    /*
     * 注册所有 API 路由
     * 在 server.cpp 中实现
     */
    void setup_routes();
};

}  // namespace backtest
