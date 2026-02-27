#pragma once

#include <string>
#include <vector>

#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/RouterHandler.h"
#include "../include/session/SessionManager.h"
#include "../auth/AuthMiddleware.h"
#include "../dao/ConversationDao.h"

namespace api
{

// GET  /api/conversations — 列表
// POST /api/conversations — 新建
class ConversationListHandler : public http::router::RouterHandler
{
public:
    explicit ConversationListHandler(http::session::SessionManager* sm)
        : sessionManager_(sm)
    {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        resp->setContentType("application/json");

        int64_t userId = 0;
        if (!auth::AuthMiddleware::check(req, resp, sessionManager_, userId))
            return;

        if (req.method() == http::HttpRequest::kGet)
        {
            auto convs = dao::ConversationDao::listByUser(userId);
            std::string json = "[";
            for (size_t i = 0; i < convs.size(); ++i)
            {
                if (i > 0) json += ",";
                json += R"({"id":)" + std::to_string(convs[i].id)
                     + R"(,"title":")" + escapeJson(convs[i].title)
                     + R"(","created_at":")" + convs[i].createdAt
                     + R"(","updated_at":")" + convs[i].updatedAt
                     + R"("})";
            }
            json += "]";
            resp->setStatusCode(http::HttpResponse::k200Ok);
            resp->setBody(json);
        }
        else if (req.method() == http::HttpRequest::kPost)
        {
            std::string body = req.getBody();
            std::string title = extractField(body, "title");
            if (title.empty()) title = "New Chat";

            int64_t convId = dao::ConversationDao::create(userId, title);
            resp->setStatusCode(http::HttpResponse::k200Ok);
            resp->setBody(R"({"ok":true,"id":)" + std::to_string(convId)
                         + R"(,"title":")" + escapeJson(title) + R"("})");
        }
    }

private:
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
            { ++pos; result += json[pos]; }
            else
            { result += json[pos]; }
            ++pos;
        }
        return result;
    }

    static std::string escapeJson(const std::string& s)
    {
        std::string r;
        for (char c : s)
        {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n";  break;
                default:   r += c;      break;
            }
        }
        return r;
    }

    http::session::SessionManager* sessionManager_;
};


// DELETE /api/conversations/:id
// PUT    /api/conversations/:id
class ConversationDetailHandler : public http::router::RouterHandler
{
public:
    explicit ConversationDetailHandler(http::session::SessionManager* sm)
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

        if (req.method() == http::HttpRequest::kDelete)
        {
            bool ok = dao::ConversationDao::remove(convId, userId);
            resp->setStatusCode(http::HttpResponse::k200Ok);
            resp->setBody(ok ? R"({"ok":true})" : R"({"ok":false,"error":"not found"})");
        }
        else if (req.method() == http::HttpRequest::kPut)
        {
            std::string body = req.getBody();
            std::string title = extractField(body, "title");
            if (title.empty())
            {
                resp->setStatusCode(http::HttpResponse::k400BadRequest);
                resp->setBody(R"({"error":"title required"})");
                return;
            }
            bool ok = dao::ConversationDao::updateTitle(convId, userId, title);
            resp->setStatusCode(http::HttpResponse::k200Ok);
            resp->setBody(ok ? R"({"ok":true})" : R"({"ok":false,"error":"not found"})");
        }
    }

private:
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
            { ++pos; result += json[pos]; }
            else
            { result += json[pos]; }
            ++pos;
        }
        return result;
    }

    http::session::SessionManager* sessionManager_;
};

} // namespace api
