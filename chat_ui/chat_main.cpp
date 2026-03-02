/**
 * chat_main.cpp
 *
 * 完整的聊天服务器，集成：
 * - 用户注册/登录/登出 (Session)
 * - 多会话管理 (CRUD)
 * - 消息持久化 (MySQL)
 * - SSE 流式推送 + 多模型工厂 (策略模式 + 注册式工厂)
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
#include "session/RedisSessionStorage.h"
#include "utils/MysqlUtil.h"

#include "sse/ChatSseHandler.h"
#include "auth/AuthHandler.h"
#include "auth/AuthMiddleware.h"
#include "api/ConversationHandler.h"
#include "api/MessageHandler.h"

// ─── 多模型工厂（触发所有厂商自动注册）────────────────────
#include "ai/ModelRegister.h"
#include "ai/AIConfig.h"
#include "ai/AIFactory.h"

// ─── MCP 内置工具 ─────────────────────────────────────────────
#include "mcp/BuiltinTools.h"

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

    // ─── 多模型配置加载 ──────────────────────────────────
    // 优先读 config.json，找不到则从环境变量构造默认配置
    ai::AIConfig aiConfig;
    bool configLoaded = aiConfig.load("./config.json") ||
                        aiConfig.load("../chat_ui/config.json");

    if (!configLoaded)
    {
        std::cout << "[AI] config.json not found, building config from env vars\n";

        // 兼容原有环境变量，自动构造一个 "default" 厂商配置
        // 这样不需要改任何部署脚本就能运行
        ai::ModelConfig envCfg;
        envCfg.baseUrl   = getEnv("ANTHROPIC_BASE_URL", "https://renrenai.chat");
        envCfg.apiKey    = getEnv("ANTHROPIC_AUTH_TOKEN", "");
        envCfg.model     = getEnv("ANTHROPIC_MODEL",      "claude-sonnet-4-5-20250929");
        envCfg.maxTokens = 4096;
        envCfg.timeout   = 120;

        // 去掉末尾斜杠
        while (!envCfg.baseUrl.empty() && envCfg.baseUrl.back() == '/')
            envCfg.baseUrl.pop_back();

        // 动态注册 "claude"，复用已读取的环境变量配置
        // ClaudeStrategy = OpenAI 兼容接口，与原 LlmClient(isOpenAI=true) 行为一致
        ai::AIFactory::instance().registerModel(
            "claude",
            [envCfg](const ai::ModelConfig&) {
                return std::make_unique<ai::ClaudeStrategy>(envCfg);
            }
        );
        aiConfig.setFallbackModel("claude");
    }

    // 打印已注册的模型列表
    auto modelList = ai::AIFactory::instance().listModels();
    std::cout << "╔══════════════════════════════════════╗\n"
              << "║   LLM Chat Server (Multi-Model SSE)  ║\n"
              << "╠══════════════════════════════════════╣\n"
              << "║ Port   : " << port
              << std::string(28 - std::to_string(port).size(), ' ') << "║\n"
              << "║ Default: " << aiConfig.defaultModel().substr(0, 28)
              << std::string(28 - std::min((int)aiConfig.defaultModel().size(), 28), ' ') << "║\n"
              << "║ Models : ";
    std::string modelStr;
    for (auto& m : modelList) modelStr += m + " ";
    std::cout << modelStr.substr(0, 28)
              << std::string(28 - std::min((int)modelStr.size(), 28), ' ') << "║\n"
              << "╚══════════════════════════════════════╝\n"
              << "  Open: http://localhost:" << port << "\n\n";

    // ─── 注册 MCP 内置工具 ───────────────────────────────
    mcp::registerBuiltinTools();

    // ─── 创建 HTTP Server ────────────────────────────────
    http::HttpServer server(port, "ChatServer");
    server.setThreadNum(4);

    // ─── Session 管理器 ──────────────────────────────────
    std::string redisUri = getEnv("REDIS_URI", "tcp://127.0.0.1:6379");
    auto sessionStorage = std::make_unique<http::session::RedisSessionStorage>(redisUri, 3600);
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

    // 健康检查（新增返回可用模型列表）
    server.Get("/api/health", [](const http::HttpRequest&, http::HttpResponse* resp) {
        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setContentType("application/json");

        auto models = ai::AIFactory::instance().listModels();
        std::string modelsJson = "[";
        for (size_t i = 0; i < models.size(); ++i)
        {
            if (i > 0) modelsJson += ",";
            modelsJson += "\"" + models[i] + "\"";
        }
        modelsJson += "]";

        resp->setBody(R"({"status":"ok","models":)" + modelsJson + "}");
    });

    // Session 检查接口（不变）
    server.Get("/api/auth/me", [sm](const http::HttpRequest& req, http::HttpResponse* resp) {
        resp->setContentType("application/json");

        int64_t userId = 0;
        if (!auth::AuthMiddleware::check(req, resp, sm, userId))
            return;

        std::string username;
        auto session = sm->getSession(req, resp);
        if (session)
            username = session->getValue("username");

        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setBody(R"({"ok":true,"username":")" + username + R"("})");
    });

    // 认证路由（不变）
    server.Post("/api/auth/register", std::make_shared<auth::RegisterHandler>(sm));
    server.Post("/api/auth/login",    std::make_shared<auth::LoginHandler>(sm));
    server.Post("/api/auth/logout",   std::make_shared<auth::LogoutHandler>(sm));

    // 会话 CRUD（不变）
    auto convListHandler   = std::make_shared<api::ConversationListHandler>(sm);
    auto convDetailHandler = std::make_shared<api::ConversationDetailHandler>(sm);

    server.Get("/api/conversations",  convListHandler);
    server.Post("/api/conversations", convListHandler);

    server.addRoute(http::HttpRequest::kPut,
                    "/api/conversations/:id", convDetailHandler);
    server.addRoute(http::HttpRequest::kDelete,
                    "/api/conversations/:id", convDetailHandler);

    auto msgHandler = std::make_shared<api::MessageHandler>(sm);
    server.addRoute(http::HttpRequest::kGet,
                    "/api/conversations/:id/messages", msgHandler);

    // ─── SSE 聊天流（接入多模型工厂）────────────────────
    // ChatSseHandler 现在接收 AIConfig 而不是 LlmConfig
    auto chatHandler = std::make_shared<http::sse::ChatSseHandler>(aiConfig, sm);
    server.Post("/api/chat/stream", chatHandler);

    // ─── 启动 ────────────────────────────────────────────
    server.start();

    return 0;
}
