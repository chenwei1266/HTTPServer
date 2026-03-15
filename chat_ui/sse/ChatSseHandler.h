#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <unordered_map>

#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/RouterHandler.h"
#include "../include/session/SessionManager.h"
#include "../auth/AuthMiddleware.h"
#include "../dao/ConversationDao.h"
#include "../dao/MessageDao.h"
#include "SseManager.h"

// ─── 替换原来的 LlmClient.h ──────────────────────────────────
#include "../ai/AIConfig.h"
#include "../ai/AIFactory.h"

// ─── MCP 工具调用 ─────────────────────────────────────────────
#include "../mcp/MCPAgent.h"

namespace http
{
namespace sse
{

class ChatSseHandler : public router::RouterHandler
{
public:
    // 构造函数：接收 AIConfig（替换原来的 LlmConfig）
    explicit ChatSseHandler(const ai::AIConfig& aiConfig,
                            session::SessionManager* sm = nullptr)
        : aiConfig_(aiConfig)
        , sessionManager_(sm)
    {}

    void setSessionManager(session::SessionManager* sm)
    {
        sessionManager_ = sm;
    }

    void handle(const HttpRequest& req, HttpResponse* resp) override
    {
        // ─── OPTIONS 预检 ────────────────────────────────
        if (req.method() == HttpRequest::kOptions)
        {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->addHeader("Access-Control-Allow-Origin",  "*");
            resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
            return;
        }

        if (req.method() != HttpRequest::kPost)
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setContentType("application/json");
            resp->setBody(R"({"error":"Method not allowed"})");
            return;
        }

        std::string body = req.getBody();
        if (body.empty())
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setContentType("application/json");
            resp->setBody(R"({"error":"Empty body"})");
            return;
        }

        // ─── 解析消息 ────────────────────────────────────
        auto messages = parseMessages(body);
        if (messages.empty())
        {
            resp->setStatusCode(HttpResponse::k400BadRequest);
            resp->setContentType("application/json");
            resp->setBody(R"({"error":"No messages provided"})");
            return;
        }

        // ─── 鉴权 & 会话管理（逻辑不变）────────────────
        int64_t userId         = 0;
        int64_t conversationId = 0;

        if (sessionManager_)
        {
            userId = auth::AuthMiddleware::getUserId(req, resp, sessionManager_);
        }

        std::string convIdStr = extractNumber(body, "conversation_id");
        if (!convIdStr.empty())
        {
            try { conversationId = std::stoll(convIdStr); }
            catch (...) { conversationId = 0; }
        }

        if (userId > 0)
        {
            if (conversationId > 0)
            {
                auto conv = dao::ConversationDao::findById(conversationId, userId);
                if (conv.id == 0)
                    conversationId = 0;
            }

            if (conversationId == 0)
            {
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

            if (conversationId > 0)
            {
                // 只插入本次新增的最后一条 user 消息，不重复插入历史
                for (auto it = messages.rbegin(); it != messages.rend(); ++it)
                {
                    if (it->role == "user")
                    {
                        dao::MessageDao::insert(conversationId, "user", it->content);
                        break;
                    }
                }
            }
        }

        // ─── SSE 握手 ────────────────────────────────────
        auto conn = conn_;
        if (!conn || !conn->connected())
        {
            resp->setStatusCode(HttpResponse::k500InternalServerError);
            resp->setContentType("application/json");
            resp->setBody(R"({"error":"Connection lost"})");
            return;
        }

        conn->send(buildSseHandshake());

        std::string userIdStr = userId > 0 ? std::to_string(userId) : "";
        std::string connId = SseManager::instance().addConnection(conn, userIdStr);
        auto sseConn = SseManager::instance().getConnection(connId);
        if (!sseConn) return;

        if (conversationId > 0)
        {
            sseConn->send(
                R"({"conversation_id":)" + std::to_string(conversationId) + "}",
                "meta");
        }

        // ─── 选择模型 ────────────────────────────────────
        // 1. 从请求体取前端传来的值（可能是厂商key 或 完整模型名）
        std::string modelField = extractField(body, "model");

        // 2. 模型名 → 厂商key 映射表
        //    前端传来的是具体模型名时，在这里转换成工厂注册的 key
        //    如果前端已经传厂商key（如 "claude"），直接命中；
        //    如果传的是模型名（如 "claude-sonnet-4-5-20250929"），也能正确路由
        static const std::unordered_map<std::string, std::string> modelToProvider = {
            // Claude 系列
            {"claude-sonnet-4-5-20250929",      "claude"},
            {"claude-sonnet-4-6",               "claude"},
            {"claude-opus-4-6",                 "claude"},
            {"claude-3-5-sonnet-20241022",       "claude"},
            {"claude-3-opus-20240229",           "claude"},
            {"claude-3-haiku-20240307",          "claude"},
            // 通义千问系列
            {"qwen-plus",                        "qwen"},
            {"qwen-turbo",                       "qwen"},
            {"qwen-max",                         "qwen"},
            {"qwen2.5-72b-instruct",             "qwen"},
            // 豆包系列（ep-xxx 格式由用户自行在 config.json 配置，无法枚举）
            // 文心系列
            {"ernie-speed-128k",                 "wenxin"},
            {"ernie-4.0-turbo-8k",               "wenxin"},
            {"ernie-3.5-8k",                     "wenxin"},
        };

        std::string modelKey;
        if (!modelField.empty())
        {
            // 先查映射表
            auto it = modelToProvider.find(modelField);
            if (it != modelToProvider.end())
                modelKey = it->second;      // 命中：模型名 → 厂商key
            else
                modelKey = modelField;      // 未命中：假设前端传的就是厂商key
        }

        // 3. 兜底：使用 config.json 的 default_model
        if (modelKey.empty())
            modelKey = aiConfig_.defaultModel();

        // 4. 从工厂创建 Strategy
        ai::ModelConfig modelCfg = aiConfig_.getConfig(modelKey);
        auto strategy = ai::AIFactory::instance().tryCreateModel(modelKey, modelCfg);

        if (!strategy)
        {
            sseConn->send(
                R"({"error":"unknown model: )" + modelKey + R"("})",
                "error");
            SseManager::instance().removeConnection(connId);
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->markAsSseUpgraded();
            return;
        }

        // 通知前端当前使用的模型名（方便前端展示）
        sseConn->send(
            R"({"model":")" + strategy->getModelName() + R"(","provider":")"
            + strategy->getProviderName() + R"("})",
            "meta");

        // ─── MCP 工具调用 + 流式输出 ────────────────────
        auto fullReply      = std::make_shared<std::string>();
        auto capturedConvId = conversationId;
        auto capturedUserId = userId;
        auto capturedUserIdStr = userIdStr;

        // strategy / agent 用 shared_ptr 持有，在 lambda 里安全捕获
        auto strategyPtr = std::shared_ptr<ai::AIStrategy>(std::move(strategy));
        auto agentPtr    = std::make_shared<mcp::MCPAgent>(3); // 最多 3 轮工具调用

        std::thread([agentPtr, strategyPtr, messages,
                     sseConn, fullReply, capturedConvId, capturedUserId,
                     capturedUserIdStr, connId]() {
            agentPtr->chat(
                strategyPtr.get(),
                messages,
                [sseConn, fullReply, capturedUserIdStr, connId](const std::string& token) {
                    std::string data = R"({"token":")" + escapeJson(token) + R"("})";
                    if (!capturedUserIdStr.empty())
                        SseManager::instance().publishToUser(capturedUserIdStr, data, connId);
                    else if (sseConn && !sseConn->isClosed())
                        sseConn->send(data);
                    fullReply->append(token);
                },
                // onDone：入库 + 关闭 SSE
                [sseConn, connId, strategyPtr, agentPtr, fullReply,
                 capturedConvId, capturedUserId, capturedUserIdStr]() {
                    if (capturedUserId > 0 && capturedConvId > 0 && !fullReply->empty())
                    {
                        dao::MessageDao::insert(capturedConvId, "assistant", *fullReply);
                        dao::ConversationDao::touch(capturedConvId);
                    }
                    if (!capturedUserIdStr.empty())
                        SseManager::instance().publishToUser(capturedUserIdStr, "[DONE]", connId);
                    else if (sseConn) sseConn->sendDone();
                    SseManager::instance().removeConnection(connId);
                },
                // onError
                [sseConn, connId, strategyPtr, agentPtr, capturedUserIdStr](const std::string& error) {
                    std::string data = R"({"error":")" + escapeJson(error) + R"("})";
                    if (!capturedUserIdStr.empty())
                        SseManager::instance().publishToUser(capturedUserIdStr, data, connId);
                    else if (sseConn && !sseConn->isClosed())
                        sseConn->send(data, "error");
                    SseManager::instance().removeConnection(connId);
                },
                // onToolCall：工具调用事件推送给前端
                [sseConn, capturedUserIdStr, connId](const std::string& toolName, const std::string& result) {
                    std::string payload =
                        R"({"tool":")" + toolName + R"(","result":")" +
                        escapeJson(result) + R"("})";
                    if (!capturedUserIdStr.empty())
                        SseManager::instance().publishToUser(capturedUserIdStr, payload, connId);
                    else if (sseConn && !sseConn->isClosed())
                        sseConn->send(payload, "tool");
                }
            );
        }).detach();

        resp->setStatusCode(HttpResponse::k200Ok);
        resp->markAsSseUpgraded();
    }

private:
    // ── 消息解析（逻辑不变，只改类型从 LlmClient::Message 到 ai::Message）

    static std::vector<ai::Message> parseMessages(const std::string& body)
    {
        std::vector<ai::Message> messages;

        auto pos = body.find("\"messages\"");
        if (pos == std::string::npos) return messages;

        pos = body.find('[', pos);
        if (pos == std::string::npos) return messages;

        ++pos;
        while (pos < body.size())
        {
            while (pos < body.size() &&
                   (body[pos] == ' ' || body[pos] == '\n' ||
                    body[pos] == '\r' || body[pos] == '\t'))
                ++pos;

            if (pos >= body.size()) break;
            if (body[pos] == ']') break;
            if (body[pos] != '{') { ++pos; continue; }

            size_t objEnd = findObjectEnd(body, pos);
            std::string obj = body.substr(pos, objEnd - pos + 1);

            ai::Message msg;
            msg.role    = extractField(obj, "role");
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

    static std::string extractField(const std::string& json,
                                    const std::string& field)
    {
        std::string key = "\"" + field + "\"";
        auto pos = json.find(key);
        if (pos == std::string::npos) return "";
        pos += key.size();
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == ':')) ++pos;
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
                    case '"': result += '"';  break;
                    case '\\':result += '\\'; break;
                    default:  result += json[pos]; break;
                }
            }
            else result += json[pos];
            ++pos;
        }
        return result;
    }

    static std::string extractNumber(const std::string& json, const std::string& field)
    {
        std::string key = "\"" + field + "\"";
        auto pos = json.find(key);
        if (pos == std::string::npos) return "";
        pos += key.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        if (pos >= json.size() || !std::isdigit((unsigned char)json[pos])) return "";
        std::string result;
        while (pos < json.size() && std::isdigit((unsigned char)json[pos]))
            result += json[pos++];
        return result;
    }

    static std::string escapeJson(const std::string& s)
    {
        std::string result;
        for (char c : s)
        {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n";  break;
                case '\r': result += "\\r";  break;
                case '\t': result += "\\t";  break;
                default:   result += c;      break;
            }
        }
        return result;
    }

private:
    ai::AIConfig             aiConfig_;       // 替换原来的 llm::LlmConfig
    session::SessionManager* sessionManager_;
};

} // namespace sse
} // namespace http
