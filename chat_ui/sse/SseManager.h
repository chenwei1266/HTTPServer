#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>

#include <muduo/net/TcpConnection.h>
#include <muduo/base/Logging.h>

namespace http
{
namespace sse
{

/**
 * SseConnection: 管理单个 SSE 连接
 * 
 * SSE 协议格式：
 *   data: <message>\n\n          // 普通数据帧
 *   data: [DONE]\n\n             // 结束帧（类 OpenAI 风格）
 *   event: <eventType>\ndata: <message>\n\n  // 带事件类型
 */
class SseConnection
{
public:
    explicit SseConnection(const muduo::net::TcpConnectionPtr& conn)
        : conn_(conn)
        , closed_(false)
    {}

    // 发送 SSE 数据帧
    void send(const std::string& data, const std::string& event = "")
    {
        if (closed_ || !conn_->connected()) 
        {
            closed_ = true;
            return;
        }

        std::string frame;
        if (!event.empty())
        {
            frame += "event: " + event + "\n";
        }
        frame += "data: " + data + "\n\n";

        conn_->send(frame);
    }

    // 发送结束帧
    void sendDone()
    {
        send("[DONE]");
        // 短暂延迟后关闭，确保数据发送完毕
        closed_ = true;
    }

    // 发送心跳（防止连接超时）
    void sendHeartbeat()
    {
        if (closed_ || !conn_->connected()) return;
        conn_->send(": heartbeat\n\n");
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
 * SseManager: 管理所有 SSE 连接
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

    // 注册一个新的 SSE 连接，返回连接 ID
    ConnectionId addConnection(const muduo::net::TcpConnectionPtr& conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sseConn = std::make_shared<SseConnection>(conn);
        std::string id = conn->name();
        connections_[id] = sseConn;
        LOG_INFO << "SSE connection added: " << id;
        return id;
    }

    // 获取连接
    SseConnectionPtr getConnection(const ConnectionId& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(id);
        if (it != connections_.end()) return it->second;
        return nullptr;
    }

    // 移除连接
    void removeConnection(const ConnectionId& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(id);
        LOG_INFO << "SSE connection removed: " << id;
    }

    // 清理已关闭的连接
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
    mutable std::mutex                          mutex_;
    std::map<ConnectionId, SseConnectionPtr>   connections_;
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
