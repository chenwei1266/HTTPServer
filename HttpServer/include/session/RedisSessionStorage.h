#pragma once

#include "SessionStorage.h"
#include "SessionManager.h"
#include <sw/redis++/redis++.h>
#include <memory>
#include <string>

namespace http {
namespace session {

class RedisSessionStorage : public SessionStorage
{
public:
    /// @param uri  例如 "tcp://127.0.0.1:6379" 或带密码 "tcp://auth:password@127.0.0.1:6379"
    /// @param maxAge  session 过期秒数，用作 Redis TTL
    RedisSessionStorage(const std::string& uri, int maxAge = 3600)
        : redis_(uri), maxAge_(maxAge) {}

    void save(std::shared_ptr<Session> session) override
    {
        std::string key = "session:" + session->getId();

        // 把 Session 内部的 kv 数据全部写入 Redis Hash
        // 需要 Session 暴露数据遍历接口，下面说怎么加
        auto data = session->getAllData();
        if (data.empty()) {
            redis_.del(key);
            return;
        }

        redis_.hmset(key, data.begin(), data.end());
        redis_.expire(key, maxAge_);
    }

    std::shared_ptr<Session> load(const std::string& sessionId) override
    {
        std::string key = "session:" + sessionId;

        std::unordered_map<std::string, std::string> data;
        redis_.hgetall(key, std::inserter(data, data.begin()));

        if (data.empty())
            return nullptr;

        // 重建 Session 对象
        auto session = std::make_shared<Session>(sessionId, nullptr, maxAge_);
        for (auto& [k, v] : data)
            session->setValue(k, v);

        // 访问即续期
        redis_.expire(key, maxAge_);
        return session;
    }

    void remove(const std::string& sessionId) override
    {
        redis_.del("session:" + sessionId);
    }

private:
    sw::redis::Redis redis_;
    int maxAge_;
};

} // namespace session
} // namespace http
