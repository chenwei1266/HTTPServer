#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <map>

#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/RouterHandler.h"
#include "SseManager.h"
#include "LlmClient.h"

namespace http
{
namespace sse
{

class ChatSseHandler : public router::RouterHandler
{
public:
    explicit ChatSseHandler(const llm::LlmConfig& config = llm::LlmConfig{})
        : llmConfig_(config)
    {}

    void handle(const HttpRequest& req, HttpResponse* resp) override
    {
        // OPTIONS 预检请求（CORS）
        if (req.method() == HttpRequest::kOptions)
        {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
            return;
        }

        // 只接受 POST
        if (req.method() != HttpRequest::kPost)
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setBody("{\"error\":\"Method not allowed\"}");
            return;
        }

        // 解析请求体
        std::string body = req.getBody();
        if (body.empty())
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setBody("{\"error\":\"Empty body\"}");
            return;
        }

        // 解析消息列表
        auto messages = parseMessages(body);
        if (messages.empty())
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setBody("{\"error\":\"No messages provided\"}");
            return;
        }

        // 获取连接
        auto conn = conn_;
        if (!conn || !conn->connected())
        {
            resp->setStatusCode(HttpResponse::k500InternalServerError);
            resp->setBody("{\"error\":\"Connection lost\"}");
            return;
        }

        // 发送 SSE 握手头
        conn->send(buildSseHandshake());

        // 注册到 SseManager
        std::string connId = SseManager::instance().addConnection(conn);
        auto sseConn = SseManager::instance().getConnection(connId);
        if (!sseConn)
        {
            return;
        }

        // 确定模型（可从请求体中覆盖）
        llm::LlmConfig cfg = llmConfig_;
        std::string modelOverride = extractField(body, "model");
        if (!modelOverride.empty())
        {
            cfg.model = modelOverride;
        }

        // 关键修复：用 shared_ptr 创建 LlmClient，让 detach 线程持有所有权
        // 避免局部 LlmClient 析构后线程访问悬空成员
        auto client = std::make_shared<llm::LlmClient>(cfg);

        client->streamChat(
            messages,
            // onToken
            [sseConn, connId](const std::string& token) {
                if (sseConn && !sseConn->isClosed())
                {
                    std::string escaped = escapeJson(token);
                    sseConn->send("{\"token\":\"" + escaped + "\"}");
                }
            },
            // onDone
            [sseConn, connId, client]() {
                // 捕获 client shared_ptr 延长生命周期
                if (sseConn)
                {
                    sseConn->sendDone();
                    // 不要立即 close/shutdown，让客户端收到 [DONE] 后自行断开
                }
                SseManager::instance().removeConnection(connId);
            },
            // onError
            [sseConn, connId, client](const std::string& error) {
                if (sseConn && !sseConn->isClosed())
                {
                    sseConn->send("{\"error\":\"" + escapeJson(error) + "\"}", "error");
                }
                SseManager::instance().removeConnection(connId);
            }
        );

        // 标记为 SSE 升级，HttpServer 跳过默认响应发送
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->markAsSseUpgraded();
    }

private:
    static std::vector<llm::LlmClient::Message> parseMessages(const std::string& body)
    {
        std::vector<llm::LlmClient::Message> messages;

        auto pos = body.find("\"messages\"");
        if (pos == std::string::npos) return messages;

        pos = body.find('[', pos);
        if (pos == std::string::npos) return messages;

        ++pos;
        while (pos < body.size())
        {
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\n' || body[pos] == '\r' || body[pos] == '\t')) ++pos;

            if (pos >= body.size()) break;
            if (body[pos] == ']') break;
            if (body[pos] != '{') { ++pos; continue; }

            size_t objEnd = findObjectEnd(body, pos);
            std::string obj = body.substr(pos, objEnd - pos + 1);

            llm::LlmClient::Message msg;
            msg.role = extractField(obj, "role");
            msg.content = extractField(obj, "content");

            if (!msg.role.empty() && !msg.content.empty())
                messages.push_back(msg);

            pos = objEnd + 1;
            while (pos < body.size() && body[pos] != '{' && body[pos] != ']') ++pos;
        }

        return messages;
    }

    static size_t findObjectEnd(const std::string& s, size_t start)
    {
        int depth = 0;
        bool inStr = false;
        for (size_t i = start; i < s.size(); ++i)
        {
            if (inStr)
            {
                if (s[i] == '\\') { ++i; continue; }
                if (s[i] == '"') inStr = false;
            }
            else
            {
                if (s[i] == '"') inStr = true;
                else if (s[i] == '{') ++depth;
                else if (s[i] == '}') { --depth; if (depth == 0) return i; }
            }
        }
        return s.size() - 1;
    }

    static std::string extractField(const std::string& json, const std::string& field)
    {
        std::string key = "\"" + field + "\"";
        auto pos = json.find(key);
        if (pos == std::string::npos) return "";
        pos += key.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        if (pos >= json.size() || json[pos] != '"') return "";
        ++pos;
        std::string result;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                ++pos;
                switch (json[pos]) {
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    default: result += json[pos]; break;
                }
            }
            else result += json[pos];
            ++pos;
        }
        return result;
    }

    static std::string escapeJson(const std::string& s)
    {
        std::string result;
        for (char c : s)
        {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }

private:
    llm::LlmConfig llmConfig_;
};

} // namespace sse
} // namespace http
