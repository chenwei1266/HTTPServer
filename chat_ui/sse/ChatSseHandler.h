#pragma once

#include <string>
#include <vector>
#include <memory>

#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/RouterHandler.h"
#include "../include/session/SessionManager.h"
#include "../auth/AuthMiddleware.h"
#include "../dao/ConversationDao.h"
#include "../dao/MessageDao.h"
#include "SseManager.h"
#include "LlmClient.h"

namespace http
{
namespace sse
{

class ChatSseHandler : public router::RouterHandler
{
public:
    explicit ChatSseHandler(const llm::LlmConfig& config = llm::LlmConfig{},
                            session::SessionManager* sm = nullptr)
        : llmConfig_(config)
        , sessionManager_(sm)
    {}

    void setSessionManager(session::SessionManager* sm)
    {
        sessionManager_ = sm;
    }

    void handle(const HttpRequest& req, HttpResponse* resp) override
    {
        // OPTIONS 预检
        if (req.method() == HttpRequest::kOptions)
        {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
            return;
        }

        if (req.method() != HttpRequest::kPost)
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"Method not allowed"})");
            return;
        }

        std::string body = req.getBody();
        if (body.empty())
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"Empty body"})");
            return;
        }

        auto messages = parseMessages(body);
        if (messages.empty())
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"No messages provided"})");
            return;
        }

        // ─── 鉴权 & 会话 ─────────────────────────────────
        int64_t userId = 0;
        int64_t conversationId = 0;

        if (sessionManager_)
        {
            userId = auth::AuthMiddleware::getUserId(req, resp, sessionManager_);
        }

        // 从请求体提取 conversation_id
        std::string convIdStr = extractField(body, "conversation_id");
        if (!convIdStr.empty())
        {
            try { conversationId = std::stoll(convIdStr); }
            catch (...) { conversationId = 0; }
        }

        // 已登录用户：校验或自动创建会话
        if (userId > 0)
        {
            if (conversationId > 0)
            {
                auto conv = dao::ConversationDao::findById(conversationId, userId);
                if (conv.id == 0)
                    conversationId = 0; // 不属于该用户
            }

            if (conversationId == 0)
            {
                // 用最后一条 user 消息的前 30 字符作为标题
                std::string title = "New Chat";
                for (auto it = messages.rbegin(); it != messages.rend(); ++it)
                {
                    if (it->role == "user" && !it->content.empty())
                    {
                        title = it->content.substr(0, 30);
                        break;
                    }
                }
                conversationId = dao::ConversationDao::create(userId, title);
            }

            // 将本次 user 消息写入数据库
            if (conversationId > 0)
            {
                for (auto& m : messages)
                {
                    if (m.role == "user")
                        dao::MessageDao::insert(conversationId, "user", m.content);
                }
            }
        }

        // ─── SSE 握手 ────────────────────────────────────
        auto conn = conn_;
        if (!conn || !conn->connected())
        {
            resp->setStatusCode(HttpResponse::k500InternalServerError);
            resp->setBody(R"({"error":"Connection lost"})");
            return;
        }

        conn->send(buildSseHandshake());

        std::string connId = SseManager::instance().addConnection(conn);
        auto sseConn = SseManager::instance().getConnection(connId);
        if (!sseConn)
            return;

        // 如果有 conversation_id，先推送给前端
        if (conversationId > 0)
        {
            sseConn->send(R"({"conversation_id":)" + std::to_string(conversationId) + "}", "meta");
        }

        // ─── LLM 配置 ───────────────────────────────────
        llm::LlmConfig cfg = llmConfig_;
        std::string modelOverride = extractField(body, "model");
        if (!modelOverride.empty())
            cfg.model = modelOverride;

        // ─── 流式调用 ───────────────────────────────────
        // 用 shared_ptr 收集完整回复
        auto fullReply = std::make_shared<std::string>();
        auto capturedConvId = conversationId;
        auto capturedUserId = userId;

        auto client = std::make_shared<llm::LlmClient>(cfg);

        client->streamChat(
            messages,
            // onToken
            [sseConn, connId, fullReply](const std::string& token) {
                if (sseConn && !sseConn->isClosed())
                {
                    std::string escaped = escapeJson(token);
                    sseConn->send(R"({"token":")" + escaped + R"("})");
                }
                fullReply->append(token);
            },
            // onDone
            [sseConn, connId, client, fullReply, capturedConvId, capturedUserId]() {
                // 将 assistant 回复写入数据库
                if (capturedUserId > 0 && capturedConvId > 0 && !fullReply->empty())
                {
                    dao::MessageDao::insert(capturedConvId, "assistant", *fullReply);
                    dao::ConversationDao::touch(capturedConvId);
                }

                if (sseConn)
                    sseConn->sendDone();

                SseManager::instance().removeConnection(connId);
            },
            // onError
            [sseConn, connId, client](const std::string& error) {
                if (sseConn && !sseConn->isClosed())
                    sseConn->send(R"({"error":")" + escapeJson(error) + R"("})", "error");

                SseManager::instance().removeConnection(connId);
            }
        );

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
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\n'
                   || body[pos] == '\r' || body[pos] == '\t'))
                ++pos;

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
            while (pos < body.size() && body[pos] != '{' && body[pos] != ']')
                ++pos;
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
    llm::LlmConfig             llmConfig_;
    session::SessionManager*   sessionManager_;
};

} // namespace sse
} // namespace http
