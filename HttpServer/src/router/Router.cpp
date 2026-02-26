#include "../../include/router/Router.h"

namespace http
{
namespace router
{

void Router::registerHandler(HttpRequest::Method method, const std::string &path, HandlerPtr handler)
{
    RouteKey key{method, path};
    handlers_[key] = handler;
}

void Router::registerCallback(HttpRequest::Method method, const std::string &path, const HandlerCallback &callback)
{
    RouteKey key{method, path};
    callbacks_[key] = callback;
}

// ★ 新增 conn 参数版本，替换原来的 route(req, resp)
bool Router::route(const muduo::net::TcpConnectionPtr &conn,
                   const HttpRequest &req,
                   HttpResponse *resp)
{
    RouteKey key{req.method(), req.path()};

    // 1. 精确匹配 Handler
    auto handlerIt = handlers_.find(key);
    if (handlerIt != handlers_.end())
    {
        // ★ 注入连接，SSE handler 在 handle() 内可直接使用 conn_
        handlerIt->second->conn_ = conn;
        handlerIt->second->handle(req, resp);
        return true;
    }

    // 2. 精确匹配 Callback（回调函数无需注入 conn）
    auto callbackIt = callbacks_.find(key);
    if (callbackIt != callbacks_.end())
    {
        callbackIt->second(req, resp);
        return true;
    }

    // 3. 正则匹配 Handler
    std::string path = req.path();
    for (auto &routeObj : regexHandlers_)
    {
        std::smatch match;
        if (routeObj.method_ == req.method() &&
            std::regex_match(path, match, routeObj.pathRegex_))
        {
            extractPathParameters(match, const_cast<HttpRequest &>(req));
            // ★ 注入连接
            routeObj.handler_->conn_ = conn;
            routeObj.handler_->handle(req, resp);
            return true;
        }
    }

    // 4. 正则匹配 Callback
    for (auto &routeObj : regexCallbacks_)
    {
        std::smatch match;
        if (routeObj.method_ == req.method() &&
            std::regex_match(path, match, routeObj.pathRegex_))
        {
            extractPathParameters(match, const_cast<HttpRequest &>(req));
            routeObj.callback_(req, resp);
            return true;
        }
    }

    return false;
}

// ★ 保留旧签名作为兼容重载（内部委托给新版本，conn 传空）
bool Router::route(const HttpRequest &req, HttpResponse *resp)
{
    return route(muduo::net::TcpConnectionPtr{}, req, resp);
}

} // namespace router
} // namespace http