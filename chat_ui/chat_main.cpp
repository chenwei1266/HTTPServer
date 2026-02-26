/**
 * examples/chat_main.cpp
 *
 * 完整的聊天服务器示例，集成了：
 * - HttpServer (基于 muduo)
 * - SSE 流式推送
 * - LLM 流式调用 (Ollama / OpenAI 兼容 / Anthropic 中转)
 * - 聊天 UI 内嵌
 *
 * 编译：
 *   mkdir build && cd build
 *   cmake .. && make -j4
 *
 * 运行：
 *   export ANTHROPIC_AUTH_TOKEN="sk-xxx"
 *   ./chat_server [port]   # 默认 8080
 *
 * 访问：
 *   http://localhost:8080
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>

#include "http/HttpServer.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "./sse/ChatSseHandler.h"

// ─── 内嵌的 HTML 页面（从文件读取或硬编码）─────────────────
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

// 安全获取环境变量
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

    if (!loadHtml("./chat_ui.html") && !loadHtml("../examples/chat_ui.html"))
    {
        g_htmlPage = R"(<!DOCTYPE html>
<html><head><meta charset="UTF-8">
<title>Chat</title>
<style>body{background:#0e0e10;color:#e8e8ed;display:flex;align-items:center;justify-content:center;height:100vh;font-family:sans-serif;}</style>
</head><body>
<div style="text-align:center">
<h2>Chat UI</h2>
<p>请将 chat_ui.html 放到运行目录下</p>
<p style="color:#8888a0;font-size:13px">或在编译时内嵌 HTML 内容</p>
</div>
</body></html>)";
    }

    // ─── LLM 配置 ─────────────────────────────────────────
    llm::LlmConfig llmCfg;

    // === RenRen AI 中转站 (OpenAI 兼容协议) ===
    // 从环境变量读取，支持运行时配置
    llmCfg.baseUrl   = getEnv("ANTHROPIC_BASE_URL", "https://renrenai.chat");
    llmCfg.apiKey    = getEnv("ANTHROPIC_AUTH_TOKEN", "sk-CmLMhLWnfIteONPsuwp1wNgB1ZVdyQWtOcODleixYkILKPxt");
    llmCfg.model     = getEnv("ANTHROPIC_MODEL", "claude-sonnet-4-5-20250929");
    llmCfg.isOpenAI  = true;   // 中转站走 OpenAI 兼容协议
    llmCfg.maxTokens = 4096;   // Claude 必须指定
    llmCfg.timeout   = 120;

    // 去掉 baseUrl 末尾的斜杠（如果有）
    while (!llmCfg.baseUrl.empty() && llmCfg.baseUrl.back() == '/')
        llmCfg.baseUrl.pop_back();

    std::cout << "╔══════════════════════════════════════╗\n"
              << "║   LLM Chat Server (SSE Mode)         ║\n"
              << "╠══════════════════════════════════════╣\n"
              << "║ Port : " << port << std::string(30 - std::to_string(port).size(), ' ') << "║\n"
              << "║ Model: " << llmCfg.model.substr(0, 30)
              << std::string(30 - std::min((int)llmCfg.model.size(), 30), ' ') << "║\n"
              << "║ LLM  : " << llmCfg.baseUrl.substr(0, 30)
              << std::string(30 - std::min((int)llmCfg.baseUrl.size(), 30), ' ') << "║\n"
              << "║ Key  : " << llmCfg.apiKey.substr(0, 8) << "..."
              << std::string(21, ' ') << "║\n"
              << "╚══════════════════════════════════════╝\n"
              << "  Open: http://localhost:" << port << "\n\n";

    // ─── 创建 HTTP Server ────────────────────────────────
    http::HttpServer server(port, "ChatServer");
    server.setThreadNum(4);

    // ─── 路由注册 ─────────────────────────────────────────

    server.Get("/", [](const http::HttpRequest&, http::HttpResponse* resp) {
        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setContentType("text/html; charset=utf-8");
        resp->setBody(g_htmlPage);
    });

    auto chatHandler = std::make_shared<http::sse::ChatSseHandler>(llmCfg);
    server.Post("/api/chat/stream", chatHandler);

    server.Get("/api/health", [&llmCfg](const http::HttpRequest&, http::HttpResponse* resp) {
        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->addHeader("Access-Control-Allow-Origin", "*");
        std::string body = "{\"status\":\"ok\",\"model\":\"" + llmCfg.model
            + "\",\"backend\":\"" + llmCfg.baseUrl + "\"}";
        resp->setBody(body);
    });

    // ─── 启动 ─────────────────────────────────────────────
    server.start();

    return 0;
}
