/**
 * chat_main.cpp
 *
 * 完整的聊天服务器，集成：
 * - 用户注册/登录/登出 (Session)
 * - 多会话管理 (CRUD)
 * - 消息持久化 (MySQL)
 * - SSE 流式推送 + LLM 调用
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>

#include "http/HttpServer.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "session/SessionManager.h"
#include "session/SessionStorage.h"
#include "utils/MysqlUtil.h"

#include "sse/ChatSseHandler.h"
#include "auth/AuthHandler.h"
#include "auth/AuthMiddleware.h"
#include "api/ConversationHandler.h"
#include "api/MessageHandler.h"

// ─── HTML 页面 ───────────────────────────────────────────────
static std::string g_htmlPage;

static bool loadHtml(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    g_htmlPage = oss.str();
    return true;
}

static std::string getEnv(const char* name, const std::string& defaultVal = "")
{
    const char* val = std::getenv(name);
    return (val && val[0]) ? std::string(val) : defaultVal;
}

// ─── 主函数 ──────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    int port = 8080;
    if (argc > 1) port = std::atoi(argv[1]);

    // ─── 加载 HTML ───────────────────────────────────────
    if (!loadHtml("./chat_ui.html") && !loadHtml("../chat_ui/chat_ui.html"))
    {
        g_htmlPage = R"(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>Chat</title>
<style>body{background:#0e0e10;color:#e8e8ed;display:flex;align-items:center;
justify-content:center;height:100vh;font-family:sans-serif;}</style>
</head><body><div style="text-align:center">
<h2>Chat UI</h2><p>chat_ui.html not found</p>
</div></body></html>)";
    }

    // ─── 数据库初始化 ────────────────────────────────────
    std::string dbHost = getEnv("DB_HOST", "localhost");
    std::string dbUser = getEnv("DB_USER", "root");
    std::string dbPass = getEnv("DB_PASS", "123456");
    std::string dbName = getEnv("DB_NAME", "chat_app");
    int dbPoolSize     = std::atoi(getEnv("DB_POOL_SIZE", "10").c_str());

    http::MysqlUtil::init(dbHost, dbUser, dbPass, dbName, dbPoolSize);
    std::cout << "[DB] Connected to " << dbHost << "/" << dbName
              << " (pool=" << dbPoolSize << ")\n";

    // ─── LLM 配置 ───────────────────────────────────────
    llm::LlmConfig llmCfg;
    llmCfg.baseUrl   = getEnv("ANTHROPIC_BASE_URL", "https://renrenai.chat");
    llmCfg.apiKey    = getEnv("ANTHROPIC_AUTH_TOKEN", "sk-CmLMhLWnfIteONPsuwp1wNgB1ZVdyQWtOcODleixYkILKPxt");
    llmCfg.model     = getEnv("ANTHROPIC_MODEL", "claude-sonnet-4-5-20250929");
    llmCfg.isOpenAI  = true;
    llmCfg.maxTokens = 4096;
    llmCfg.timeout   = 120;

    while (!llmCfg.baseUrl.empty() && llmCfg.baseUrl.back() == '/')
        llmCfg.baseUrl.pop_back();

    std::cout << "╔══════════════════════════════════════╗\n"
              << "║   LLM Chat Server (SSE Mode)         ║\n"
              << "╠══════════════════════════════════════╣\n"
              << "║ Port : " << port
              << std::string(30 - std::to_string(port).size(), ' ') << "║\n"
              << "║ Model: " << llmCfg.model.substr(0, 30)
              << std::string(30 - std::min((int)llmCfg.model.size(), 30), ' ') << "║\n"
              << "║ LLM  : " << llmCfg.baseUrl.substr(0, 30)
              << std::string(30 - std::min((int)llmCfg.baseUrl.size(), 30), ' ') << "║\n"
              << "╚══════════════════════════════════════╝\n"
              << "  Open: http://localhost:" << port << "\n\n";

    // ─── 创建 HTTP Server ────────────────────────────────
    http::HttpServer server(port, "ChatServer");
    server.setThreadNum(4);

    // ─── Session 管理器 ──────────────────────────────────
    auto sessionStorage = std::make_unique<http::session::MemorySessionStorage>();
    auto sessionManager = std::make_unique<http::session::SessionManager>(
        std::move(sessionStorage));
    http::session::SessionManager* sm = sessionManager.get();
    server.setSessionManager(std::move(sessionManager));

    // ─── 路由注册 ────────────────────────────────────────

    // 首页
    server.Get("/", [](const http::HttpRequest&, http::HttpResponse* resp) {
        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setContentType("text/html; charset=utf-8");
        resp->setBody(g_htmlPage);
    });

    // 健康检查
    server.Get("/api/health", [&llmCfg](const http::HttpRequest&, http::HttpResponse* resp) {
        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(R"({"status":"ok","model":")" + llmCfg.model + R"("})");
    });

    // ─── Session 检查接口 ────────────────────────────────
    // GET /api/auth/me
    //   已登录 → 200  {"ok":true,"username":"xxx"}
    //   未登录 → 401  {"error":"Unauthorized"}
    // 与其他 Handler 保持一致，使用 AuthMiddleware::check()
    server.Get("/api/auth/me", [sm](const http::HttpRequest& req, http::HttpResponse* resp) {
        resp->setContentType("application/json");

        int64_t userId = 0;
        if (!auth::AuthMiddleware::check(req, resp, sm, userId))
            return; // check() 内部已设置 401 响应体，直接 return

        // 复用同一个 session，取出登录时存入的 username
        std::string username;
        auto session = sm->getSession(req, resp);
        if (session)
            username = session->getValue("username");

        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setBody(R"({"ok":true,"username":")" + username + R"("})");
    });

    // 认证路由
    server.Post("/api/auth/register", std::make_shared<auth::RegisterHandler>(sm));
    server.Post("/api/auth/login",    std::make_shared<auth::LoginHandler>(sm));
    server.Post("/api/auth/logout",   std::make_shared<auth::LogoutHandler>(sm));

    // 会话 CRUD
    auto convListHandler   = std::make_shared<api::ConversationListHandler>(sm);
    auto convDetailHandler = std::make_shared<api::ConversationDetailHandler>(sm);

    server.Get("/api/conversations",  convListHandler);
    server.Post("/api/conversations", convListHandler);

    // 正则路由: /api/conversations/:id
    server.addRoute(http::HttpRequest::kPut,
                    "/api/conversations/:id", convDetailHandler);
    server.addRoute(http::HttpRequest::kDelete,
                    "/api/conversations/:id", convDetailHandler);

    // 消息历史: GET /api/conversations/:id/messages
    auto msgHandler = std::make_shared<api::MessageHandler>(sm);
    server.addRoute(http::HttpRequest::kGet,
                    "/api/conversations/:id/messages", msgHandler);

    // SSE 聊天流
    auto chatHandler = std::make_shared<http::sse::ChatSseHandler>(llmCfg, sm);
    server.Post("/api/chat/stream", chatHandler);

    // ─── 启动 ────────────────────────────────────────────
    server.start();

    return 0;
}