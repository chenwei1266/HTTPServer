#include "../../../include/middleware/ratelimit/RateLimitMiddleware.h"
#include "../../../include/http/HttpRequest.h"
#include "../../../include/http/HttpResponse.h"
#include <muduo/base/Logging.h>

namespace http
{
namespace middleware
{

// Lua 脚本：原子地 INCR 并在首次写入时设置 TTL，返回当前计数
// KEYS[1] = rate-limit key, ARGV[1] = window seconds
static const char* kIncrScript = R"(
local c = redis.call('INCR', KEYS[1])
if c == 1 then
    redis.call('EXPIRE', KEYS[1], ARGV[1])
end
return c
)";

RateLimitMiddleware::RateLimitMiddleware(const std::string& redisUri,
                                         const RateLimitConfig& config)
    : redis_(redisUri), config_(config)
{}

void RateLimitMiddleware::before(HttpRequest& request)
{
    // 优先取 X-Forwarded-For，否则用 X-Real-IP，再退回 "unknown"
    std::string ip = request.getHeader("X-Forwarded-For");
    if (ip.empty()) ip = request.getHeader("X-Real-IP");
    if (ip.empty()) ip = "unknown";

    // 截取第一个 IP（X-Forwarded-For 可能是逗号分隔列表）
    auto comma = ip.find(',');
    if (comma != std::string::npos) ip = ip.substr(0, comma);

    std::string key = config_.keyPrefix + ip;

    long long count = 0;
    try
    {
        count = redis_.eval<long long>(kIncrScript, {key}, {std::to_string(config_.windowSeconds)});
    }
    catch (const sw::redis::Error& e)
    {
        // Redis 不可用时放行，避免限流组件成为单点故障
        LOG_WARN << "RateLimitMiddleware: Redis error: " << e.what() << ", allowing request";
        return;
    }

    LOG_DEBUG << "RateLimitMiddleware: ip=" << ip << " count=" << count
              << " limit=" << config_.maxRequests;

    if (count > config_.maxRequests)
    {
        HttpResponse resp;
        resp.setStatusLine("HTTP/1.1", HttpResponse::HttpStatusCode(429), "Too Many Requests");
        resp.setContentType("application/json");
        resp.addHeader("Retry-After", std::to_string(config_.windowSeconds));
        std::string body = "{\"error\":\"rate limit exceeded\"}";
        resp.addHeader("Content-Length", std::to_string(body.size()));
        resp.setBody(body);
        throw resp;
    }
}

} // namespace middleware
} // namespace http
