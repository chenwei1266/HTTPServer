#pragma once

#include "../Middleware.h"
#include <sw/redis++/redis++.h>
#include <string>

namespace http
{
namespace middleware
{

struct RateLimitConfig
{
    int         maxRequests { 100 };   // 窗口内最大请求数
    int         windowSeconds { 60 };  // 滑动窗口大小（秒）
    std::string keyPrefix { "rl:" };   // Redis key 前缀
};

// 基于 Redis INCR + EXPIRE 的固定窗口限流中间件（per-IP）
// before() 超限时抛出 429 HttpResponse，短路后续处理
class RateLimitMiddleware : public Middleware
{
public:
    RateLimitMiddleware(const std::string& redisUri,
                        const RateLimitConfig& config = {});

    void before(HttpRequest& request) override;
    void after(HttpResponse& response) override {}

private:
    sw::redis::Redis redis_;
    RateLimitConfig  config_;
};

} // namespace middleware
} // namespace http
