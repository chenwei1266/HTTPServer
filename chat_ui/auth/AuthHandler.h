#pragma once

#include <string>
#include <memory>

#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/RouterHandler.h"
#include "../include/session/SessionManager.h"
#include "../dao/UserDao.h"

namespace auth
{

// POST /api/auth/register
class RegisterHandler : public http::router::RouterHandler
{
public:
    explicit RegisterHandler(http::session::SessionManager* sm)
        : sessionManager_(sm)
    {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        resp->setContentType("application/json");

        std::string body = req.getBody();
        std::string username = extractField(body, "username");
        std::string password = extractField(body, "password");

        if (username.empty() || password.empty())
        {
            resp->setStatusCode(http::HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"username and password required"})");
            return;
        }

        if (username.size() < 3 || username.size() > 64)
        {
            resp->setStatusCode(http::HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"username length must be 3-64"})");
            return;
        }

        if (password.size() < 6)
        {
            resp->setStatusCode(http::HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"password length must be >= 6"})");
            return;
        }

        int64_t userId = dao::UserDao::registerUser(username, password);
        if (userId == -1)
        {
            resp->setStatusCode(http::HttpResponse::k409Conflict);
            resp->setBody(R"({"error":"username already exists"})");
            return;
        }

        // 注册成功，自动登录
        if (sessionManager_)
        {
            auto session = sessionManager_->getSession(req, resp);
            session->setValue("user_id", std::to_string(userId));
            session->setValue("username", username);
            sessionManager_->updateSession(session);
        }

        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setBody(R"({"ok":true,"user_id":)" + std::to_string(userId)
                      + R"(,"username":")" + username + R"("})");
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


// POST /api/auth/login
class LoginHandler : public http::router::RouterHandler
{
public:
    explicit LoginHandler(http::session::SessionManager* sm)
        : sessionManager_(sm)
    {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        resp->setContentType("application/json");

        std::string body = req.getBody();
        std::string username = extractField(body, "username");
        std::string password = extractField(body, "password");

        if (username.empty() || password.empty())
        {
            resp->setStatusCode(http::HttpResponse::k400BadRequest);
            resp->setBody(R"({"error":"username and password required"})");
            return;
        }

        dao::User user = dao::UserDao::login(username, password);
        if (user.id == 0)
        {
            resp->setStatusCode(http::HttpResponse::k401Unauthorized);
            resp->setBody(R"({"error":"invalid username or password"})");
            return;
        }

        if (sessionManager_)
        {
            auto session = sessionManager_->getSession(req, resp);
            session->setValue("user_id", std::to_string(user.id));
            session->setValue("username", user.username);
            sessionManager_->updateSession(session);
        }

        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setBody(R"({"ok":true,"user_id":)" + std::to_string(user.id)
                      + R"(,"username":")" + user.username + R"("})");
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


// POST /api/auth/logout
class LogoutHandler : public http::router::RouterHandler
{
public:
    explicit LogoutHandler(http::session::SessionManager* sm)
        : sessionManager_(sm)
    {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override
    {
        resp->setContentType("application/json");

        if (sessionManager_)
        {
            auto session = sessionManager_->getSession(req, resp);
            sessionManager_->destroySession(session->getId());
        }

        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setBody(R"({"ok":true})");
    }

private:
    http::session::SessionManager* sessionManager_;
};

} // namespace auth
