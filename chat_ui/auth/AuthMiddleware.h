#pragma once

#include <string>

#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/session/SessionManager.h"

namespace auth
{

class AuthMiddleware
{
public:
    // 校验是否已登录
    // 成功返回 true，userId 通过出参返回
    // 失败返回 false，并自动设置 401 响应
    static bool check(const http::HttpRequest& req,
                      http::HttpResponse* resp,
                      http::session::SessionManager* sm,
                      int64_t& outUserId)
    {
        outUserId = 0;

        if (!sm)
        {
            resp->setStatusCode(http::HttpResponse::k500InternalServerError);
            resp->setContentType("application/json");
            resp->setBody(R"({"error":"session manager not configured"})");
            return false;
        }

        auto session = sm->getSession(req, resp);
        std::string uid = session->getValue("user_id");

        if (uid.empty())
        {
            resp->setStatusCode(http::HttpResponse::k401Unauthorized);
            resp->setContentType("application/json");
            resp->setBody(R"({"error":"not logged in"})");
            return false;
        }

        outUserId = std::stoll(uid);
        return true;
    }

    // 简化版：只返回 userId，0 表示未登录（不自动写响应）
    static int64_t getUserId(const http::HttpRequest& req,
                             http::HttpResponse* resp,
                             http::session::SessionManager* sm)
    {
        if (!sm) return 0;
        auto session = sm->getSession(req, resp);
        std::string uid = session->getValue("user_id");
        if (uid.empty()) return 0;
        return std::stoll(uid);
    }
};

} // namespace auth
