#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>

#include <muduo/net/TcpConnection.h>
#include <muduo/base/Logging.h>

#include "RedisPubSub.h"

namespace http
{
namespace sse
{

class SseConnection
{
public:
    explicit SseConnection(const muduo::net::TcpConnectionPtr& conn)
        : conn_(conn)
        , closed_(false)
    {}

    // 发送 SSE 数据帧（线程安全：通过 runInLoop 投递到 I/O 线程）
    void send(const std::string& data, const std::string& event = "")
    {
        if (closed_ || !conn_->connected())
        {
            closed_ = true;
            return;
        }

        std::string frame;
        if (!event.empty())
            frame += "event: " + event + "\n";
        frame += "data: " + data + "\n\n";

        auto conn = conn_;
        conn_->getLoop()->runInLoop([conn, frame = std::move(frame)]() {
            if (conn->connected()) conn->send(frame);
        });
    }

    // 发送结束帧
    void sendDone()
    {
        if (closed_ || !conn_->connected()) { closed_ = true; return; }
        auto conn = conn_;
        conn_->getLoop()->runInLoop([conn]() {
            if (conn->connected()) conn->send("data: [DONE]\n\n");
        });
        closed_ = true;
    }

    // 发送心跳（防止连接超时）
    void sendHeartbeat()
    {
        if (closed_ || !conn_->connected()) return;
        auto conn = conn_;
        conn_->getLoop()->runInLoop([conn]() {
            if (conn->connected()) conn->send(": heartbeat\n\n");
        });
    }

    bool isClosed() const { return closed_ || !conn_->connected(); }

    muduo::net::TcpConnectionPtr getConn() const { return conn_; }

    void close()
    {
        closed_ = true;
        if (conn_->connected())
        {
            conn_->shutdown();
        }
    }

private:
    muduo::net::TcpConnectionPtr conn_;
    bool                         closed_;
};


/**
 * SseManager: 管理所有 SSE 连接，支持单机和分布式（Redis Pub/Sub）两种模式
 */
class SseManager
{
public:
    using SseConnectionPtr = std::shared_ptr<SseConnection>;
    using ConnectionId = std::string;

    static SseManager& instance()
    {
        static SseManager mgr;
        return mgr;
    }

    // 多实例部署时调用，启用 Redis Pub/Sub 跨实例转发
    void initRedis(const std::string& redisUri)
    {
        pubsub_ = std::make_unique<RedisPubSub>(redisUri);
    }

    // 注册新 SSE 连接；userId 非空时订阅对应 Redis channel
    ConnectionId addConnection(const muduo::net::TcpConnectionPtr& conn,
                               const std::string& userId = "")
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sseConn = std::make_shared<SseConnection>(conn);
        std::string id = conn->name();
        connections_[id] = sseConn;

        if (!userId.empty() && pubsub_)
        {
            std::string channel = "sse:user:" + userId;
            userChannels_[id] = channel;
            auto weakConn = std::weak_ptr<SseConnection>(sseConn);
            pubsub_->subscribe(channel, [weakConn](const std::string&, const std::string& msg) {
                if (auto c = weakConn.lock()) c->send(msg);
            });
        }

        LOG_INFO << "SSE connection added: " << id;
        return id;
    }

    SseConnectionPtr getConnection(const ConnectionId& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(id);
        if (it != connections_.end()) return it->second;
        return nullptr;
    }

    void removeConnection(const ConnectionId& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = userChannels_.find(id);
        if (it != userChannels_.end())
        {
            if (pubsub_) pubsub_->unsubscribe(it->second);
            userChannels_.erase(it);
        }
        connections_.erase(id);
        LOG_INFO << "SSE connection removed: " << id;
    }

    // 分布式模式：发布到 Redis channel（所有实例上该用户的连接都会收到）
    // 单机模式：直接写本地连接
    void publishToUser(const std::string& userId, const std::string& data,
                       const std::string& connId = "")
    {
        if (pubsub_)
        {
            pubsub_->publish("sse:user:" + userId, data);
        }
        else if (!connId.empty())
        {
            auto conn = getConnection(connId);
            if (conn) conn->send(data);
        }
    }

    void cleanup()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = connections_.begin(); it != connections_.end();)
        {
            if (it->second->isClosed())
                it = connections_.erase(it);
            else
                ++it;
        }
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }

private:
    SseManager() = default;
    mutable std::mutex                        mutex_;
    std::map<ConnectionId, SseConnectionPtr>  connections_;
    std::map<ConnectionId, std::string>       userChannels_; // connId → Redis channel
    std::unique_ptr<RedisPubSub>              pubsub_;
};


/**
 * 辅助函数：向 HttpResponse 写入 SSE 握手头（HTTP 200 + SSE headers）
 * 调用此函数后，连接保持打开，后续通过 SseConnection::send() 推送数据
 */
inline std::string buildSseHandshake()
{
    std::string resp;
    resp += "HTTP/1.1 200 OK\r\n";
    resp += "Content-Type: text/event-stream\r\n";
    resp += "Cache-Control: no-cache\r\n";
    resp += "Connection: keep-alive\r\n";
    resp += "Access-Control-Allow-Origin: *\r\n";
    resp += "Access-Control-Allow-Headers: Content-Type\r\n";
    resp += "X-Accel-Buffering: no\r\n";  // 关闭 nginx 缓冲
    resp += "\r\n";                          // 头部结束
    return resp;
}

} // namespace sse
} // namespace http
