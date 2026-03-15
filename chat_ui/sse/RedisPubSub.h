#pragma once

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <mutex>

#include <sw/redis++/redis++.h>
#include <muduo/base/Logging.h>

namespace http
{
namespace sse
{

// 封装 redis++ Subscriber，后台线程持续消费消息
// 每个 RedisPubSub 实例对应一个 Redis 订阅连接
class RedisPubSub
{
public:
    using MessageCallback = std::function<void(const std::string& channel,
                                               const std::string& message)>;

    explicit RedisPubSub(const std::string& redisUri)
        : redis_(redisUri), running_(false) {}

    ~RedisPubSub() { stop(); }

    // 订阅 channel，收到消息时调用 cb（线程安全）
    void subscribe(const std::string& channel, MessageCallback cb)
    {
        {
            std::lock_guard<std::mutex> lk(cbMutex_);
            callbacks_[channel] = std::move(cb);
        }
        // 如果后台线程还没启动，先启动
        if (!running_.load())
            start();
        // 通知 subscriber 新增订阅（通过重建 subscriber 实现）
        needRebuild_.store(true);
    }

    void unsubscribe(const std::string& channel)
    {
        std::lock_guard<std::mutex> lk(cbMutex_);
        callbacks_.erase(channel);
        needRebuild_.store(true);
    }

    // 发布消息到 channel
    void publish(const std::string& channel, const std::string& message)
    {
        try {
            redis_.publish(channel, message);
        } catch (const sw::redis::Error& e) {
            LOG_WARN << "RedisPubSub publish error: " << e.what();
        }
    }

    void stop()
    {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

private:
    void start()
    {
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    void loop()
    {
        while (running_.load())
        {
            // 每次 needRebuild_ 时重建 subscriber（重新订阅当前 channel 集合）
            std::vector<std::string> channels;
            {
                std::lock_guard<std::mutex> lk(cbMutex_);
                for (auto& kv : callbacks_) channels.push_back(kv.first);
            }

            if (channels.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }

            needRebuild_.store(false);
            try {
                auto sub = redis_.subscriber();
                sub.on_message([this](std::string channel, std::string msg) {
                    std::lock_guard<std::mutex> lk(cbMutex_);
                    auto it = callbacks_.find(channel);
                    if (it != callbacks_.end()) it->second(channel, msg);
                });
                for (auto& ch : channels) sub.subscribe(ch);

                // consume 直到需要重建或停止
                while (running_.load() && !needRebuild_.load())
                {
                    try { sub.consume(); }
                    catch (const sw::redis::TimeoutError&) { /* 正常超时，继续 */ }
                    catch (const sw::redis::Error& e) {
                        LOG_WARN << "RedisPubSub consume error: " << e.what();
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        break;
                    }
                }
            } catch (const sw::redis::Error& e) {
                LOG_WARN << "RedisPubSub subscriber error: " << e.what();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    sw::redis::Redis redis_;
    std::thread thread_;
    std::atomic<bool> running_;
    std::atomic<bool> needRebuild_{false};

    std::mutex cbMutex_;
    std::unordered_map<std::string, MessageCallback> callbacks_;
};

} // namespace sse
} // namespace http
