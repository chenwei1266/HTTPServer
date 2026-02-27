#pragma once

#include <string>
#include <vector>

#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/RouterHandler.h"
#include "../include/session/SessionManager.h"
#include "../auth/AuthMiddleware.h"
#include "../dao/ConversationDao.h"
#include "../dao/MessageDao.h"

namespace api
{

// GET /api/conversations/:id/messages
class MessageHandler : public http::router::RouterHandler
{
public:
    explicit MessageHandler(http::session::SessionManager* sm)
        : sessionManager_(sm)
    {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        resp->setContentType("application/json");

        int64_t userId = 0;
        if (!auth::AuthMiddleware::check(req, resp, sessionManager_, userId))
            return;

        std::string idStr = req.getPathParameters("param1");
        if (idStr.empty())
        {
            resp->setStatusCode(http::HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"missing conversation id"})");
            return;
        }
        int64_t convId = std::stoll(idStr);

        // 校验会话归属
        auto conv = dao::ConversationDao::findById(convId, userId);
        if (conv.id == 0)
        {
            resp->setStatusCode(http::HttpResponse::k404NotFound);
            resp->setBody(R"({"error":"conversation not found"})");
            return;
        }

        auto messages = dao::MessageDao::listByConversation(convId);
        std::string json = "[";
        for (size_t i = 0; i < messages.size(); ++i)
        {
            if (i > 0) json += ",";
            json += R"({"id":)" + std::to_string(messages[i].id)
                 + R"(,"role":")" + messages[i].role
                 + R"(","content":")" + escapeJson(messages[i].content)
                 + R"(","created_at":")" + messages[i].createdAt
                 + R"("})";
        }
        json += "]";

        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setBody(json);
    }

private:
    static std::string escapeJson(const std::string& s)
    {
        std::string r;
        for (char c : s)
        {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n";  break;
                case '\r': r += "\\r";  break;
                case '\t': r += "\\t";  break;
                default:   r += c;      break;
            }
        }
        return r;
    }

    http::session::SessionManager* sessionManager_;
};

} // namespace api
