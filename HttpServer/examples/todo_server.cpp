/**
 * ╔══════════════════════════════════════════════════════════╗
 * ║          Todo List — HttpServer 框架使用示例             ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * 【框架关键注意事项】
 *   HttpResponse::appendToBuffer() 第一行就读取 httpVersion_：
 *     snprintf(buf, sizeof buf, "%s %d ", httpVersion_.c_str(), statusCode_);
 *   如果 httpVersion_ 为空，状态行变成 " 0 "，浏览器解析失败直接断开
 *   连接，日志里就会出现 SO_ERROR = 32 Broken pipe。
 *
 *   ✅ 正确做法：必须调用 setStatusLine() 同时设置三个字段
 *   ❌ 错误做法：只调用 setStatusCode() + setStatusMessage()
 *
 * 提供三个 HTTP 接口：
 *   GET  /        → 返回 todo.html 页面
 *   GET  /todos   → 返回所有 todo（JSON 数组）
 *   POST /todos   → 新增 todo（body: {"text":"..."}）
 */

#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <sstream>

#include "../include/http/HttpServer.h"
#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/Router.h"

// ============================================================
// § 0  业务数据
// ============================================================
std::vector<std::string> g_todos;
std::mutex               g_mutex;

// ============================================================
// § 1  工具函数
// ============================================================

static std::string parseTextField(const std::string& body)
{
    auto pos = body.find("\"text\"");
    if (pos == std::string::npos) return "";
    auto colon = body.find(':', pos + 6);
    if (colon == std::string::npos) return "";
    auto start = body.find('"', colon + 1);
    if (start == std::string::npos) return "";
    start++;
    auto end = body.find('"', start);
    if (end == std::string::npos) return "";
    return body.substr(start, end - start);
}

static std::string todosToJson()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string json = "[";
    for (size_t i = 0; i < g_todos.size(); ++i)
    {
        json += "\"" + g_todos[i] + "\"";
        if (i + 1 < g_todos.size()) json += ",";
    }
    json += "]";
    return json;
}

static std::string readFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ============================================================
// § 2  路由处理器
//       ⚠️  每个 handle() 必须用 setStatusLine()，
//           否则 httpVersion_ 为空导致 Broken pipe。
// ============================================================

class IndexHandler : public http::router::RouterHandler
{
public:
    explicit IndexHandler(const std::string& htmlPath) : htmlPath_(htmlPath) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        std::string html = readFile(htmlPath_);
        if (html.empty())
        {
            resp->setStatusLine("HTTP/1.1", http::HttpResponse::k404NotFound, "Not Found");
            resp->setContentType("text/plain; charset=utf-8");
            resp->addHeader("Connection", "close");
            std::string notFoundBody = "\xe6\x89\xbe\xe4\xb8\x8d\xe5\x88\xb0 todo.html\xef\xbc\x8c\xe8\xaf\xb7\xe7\xa1\xae\xe8\xae\xa4\xe6\x96\x87\xe4\xbb\xb6\xe8\xb7\xaf\xe5\xbe\x84\xe3\x80\x82";
            resp->addHeader("Content-Length", std::to_string(notFoundBody.size()));
            resp->setBody(notFoundBody);
            return;
        }
        resp->setStatusLine("HTTP/1.1", http::HttpResponse::k200Ok, "OK");
        resp->setContentType("text/html; charset=utf-8");
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Connection", "close");
        resp->addHeader("Content-Length", std::to_string(html.size()));
        resp->setBody(html);
    }

private:
    std::string htmlPath_;
};

class GetTodosHandler : public http::router::RouterHandler
{
public:
    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        resp->setStatusLine("HTTP/1.1", http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json; charset=utf-8");
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Connection", "close");
        std::string jsonBody = todosToJson();
        resp->addHeader("Content-Length", std::to_string(jsonBody.size()));
        resp->setBody(jsonBody);
    }
};

class PostTodoHandler : public http::router::RouterHandler
{
public:
    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        std::string text = parseTextField(req.getBody());
        if (text.empty())
        {
            resp->setStatusLine("HTTP/1.1", http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Connection", "close");
            std::string errBody = "{\"error\":\"missing or empty text field\"}";
            resp->addHeader("Content-Length", std::to_string(errBody.size()));
            resp->setBody(errBody);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_todos.push_back(text);
        }
        resp->setStatusLine("HTTP/1.1", http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json; charset=utf-8");
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Connection", "close");
        std::string okBody = "{\"status\":\"created\",\"text\":\"" + text + "\"}";
        resp->addHeader("Content-Length", std::to_string(okBody.size()));
        resp->setBody(okBody);
    }
};

class OptionsHandler : public http::router::RouterHandler
{
public:
    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        resp->setStatusLine("HTTP/1.1", http::HttpResponse::k200Ok, "OK");
        resp->addHeader("Access-Control-Allow-Origin",  "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
        resp->addHeader("Connection", "close");
        resp->addHeader("Content-Length", "0");
        resp->setBody("");
    }
};

// ============================================================
// § 3  TodoServer
// ============================================================
class TodoServer
{
public:
    TodoServer(int port, const std::string& name, const std::string& htmlPath)
        : server_(port, name), htmlPath_(htmlPath)
    {
        initRouter();
    }

    void start()
    {
        server_.setThreadNum(2);
        printf("========================================\n");
        printf("  Todo Server 已启动\n");
        printf("  浏览器访问: http://localhost:8080/\n");
        printf("========================================\n");
        server_.start();
    }

private:
    void initRouter()
    {
        server_.Get("/",       std::make_shared<IndexHandler>(htmlPath_));
        server_.Get("/todos",  std::make_shared<GetTodosHandler>());
        server_.Post("/todos", std::make_shared<PostTodoHandler>());
    }

    http::HttpServer server_;
    std::string      htmlPath_;
};

// ============================================================
// § 4  main
// ============================================================
int main(int argc, char* argv[])
{
    g_todos.push_back("阅读 HttpServer 框架源码");
    g_todos.push_back("完成今天的任务清单");

    std::string htmlPath = "./todo.html";
    if (argc > 1) htmlPath = argv[1];

    TodoServer server(8080, "TodoServer", htmlPath);
    server.start();
    return 0;
}