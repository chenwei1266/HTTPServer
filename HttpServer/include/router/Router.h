#pragma once
#include <iostream>
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>
#include <regex>
#include <vector>

#include "RouterHandler.h"
#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"

namespace http
{
namespace router
{

class Router
{
public:
    using HandlerPtr = std::shared_ptr<RouterHandler>;
    using HandlerCallback = std::function<void(const HttpRequest &, HttpResponse *)>;

    struct RouteKey
    {
        HttpRequest::Method method;
        std::string path;

        bool operator==(const RouteKey &other) const
        {
            return method == other.method && path == other.path;
        }
    };

    struct RouteKeyHash
    {
        size_t operator()(const RouteKey &key) const
        {
            size_t methodHash = std::hash<int>{}(static_cast<int>(key.method));
            size_t pathHash = std::hash<std::string>{}(key.path);
            return methodHash * 31 + pathHash;
        }
    };

    void registerHandler(HttpRequest::Method method, const std::string &path, HandlerPtr handler);
    void registerCallback(HttpRequest::Method method, const std::string &path, const HandlerCallback &callback);

    void addRegexHandler(HttpRequest::Method method, const std::string &path, HandlerPtr handler)
    {
        std::regex pathRegex = convertToRegex(path);
        regexHandlers_.emplace_back(method, pathRegex, handler);
    }

    void addRegexCallback(HttpRequest::Method method, const std::string &path, const HandlerCallback &callback)
    {
        std::regex pathRegex = convertToRegex(path);
        regexCallbacks_.emplace_back(method, pathRegex, callback);
    }

    // ★ 主要版本：携带 conn，供 SSE handler 注入连接
    bool route(const muduo::net::TcpConnectionPtr &conn,
               const HttpRequest &req,
               HttpResponse *resp);

    // ★ 兼容旧代码的重载（conn 传空指针，普通路由不受影响）
    bool route(const HttpRequest &req, HttpResponse *resp);

private:
    std::regex convertToRegex(const std::string &pathPattern)
    {
        std::string regexPattern = "^" + std::regex_replace(pathPattern, std::regex(R"(/:([^/]+))"), R"(/([^/]+))") + "$";
        return std::regex(regexPattern);
    }

    void extractPathParameters(const std::smatch &match, HttpRequest &request)
    {
        for (size_t i = 1; i < match.size(); ++i)
        {
            request.setPathParameters("param" + std::to_string(i), match[i].str());
        }
    }

private:
    struct RouteCallbackObj
    {
        HttpRequest::Method method_;
        std::regex          pathRegex_;
        HandlerCallback     callback_;
        RouteCallbackObj(HttpRequest::Method method, std::regex pathRegex, const HandlerCallback &callback)
            : method_(method), pathRegex_(pathRegex), callback_(callback) {}
    };

    struct RouteHandlerObj
    {
        HttpRequest::Method method_;
        std::regex          pathRegex_;
        HandlerPtr          handler_;
        RouteHandlerObj(HttpRequest::Method method, std::regex pathRegex, HandlerPtr handler)
            : method_(method), pathRegex_(pathRegex), handler_(handler) {}
    };

    std::unordered_map<RouteKey, HandlerPtr, RouteKeyHash>      handlers_;
    std::unordered_map<RouteKey, HandlerCallback, RouteKeyHash> callbacks_;
    std::vector<RouteHandlerObj>                                regexHandlers_;
    std::vector<RouteCallbackObj>                               regexCallbacks_;
};

} // namespace router
} // namespace http