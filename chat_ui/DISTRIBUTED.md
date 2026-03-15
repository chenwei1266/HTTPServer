# 分布式扩展说明

## 新增文件

```
chat_ui/sse/RedisPubSub.h   — Redis Pub/Sub 封装
```

## 修改文件

```
chat_ui/sse/SseManager.h    — 新增 initRedis / publishToUser，addConnection 支持 userId
chat_ui/sse/ChatSseHandler.h — token 推送改走 publishToUser
chat_ui/chat_main.cpp        — 启动时调用 SseManager::initRedis
```

---

## 核心机制

### 问题：多实例部署时 SSE 连接不共享

用户的 SSE 长连接建立在某一个实例上，但 AI Worker 可能在任意实例处理请求。
单机模式下直接写 `TcpConnectionPtr` 没问题，多实例时 Worker 找不到连接。

### 解法：Redis Pub/Sub 作为消息总线

```
实例 A（用户连接在此）          实例 B（AI 处理在此）
┌─────────────────────┐        ┌─────────────────────┐
│ SseManager          │        │ ChatSseHandler       │
│  subscribe(         │        │  publishToUser(      │
│   "sse:user:42",    │◄───────│   "42",              │
│   → send to conn)   │ Redis  │   token)             │
└─────────────────────┘ Pub/Sub└─────────────────────┘
```

每个 token 通过 `redis.publish("sse:user:{userId}", data)` 广播，
持有该用户 SSE 连接的实例订阅了这个 channel，收到后直接写 TCP 连接。

---

## 新增组件详解

### RedisPubSub.h

封装 `redis++` 的 `Subscriber`，后台单线程持续 `consume()`。

- `subscribe(channel, callback)` — 订阅 channel，收到消息时调用 callback
- `unsubscribe(channel)` — 取消订阅（用户断开 SSE 时调用）
- `publish(channel, message)` — 发布消息（AI Worker 调用）
- 支持动态增删订阅（通过 `needRebuild_` 标志重建 Subscriber）
- Redis 断线自动重连（sleep 1s 后重试）

### SseManager 新增接口

```cpp
// 启用分布式模式（单机部署不调用此方法即可）
void initRedis(const std::string& redisUri);

// addConnection 新增 userId 参数
ConnectionId addConnection(const TcpConnectionPtr& conn, const std::string& userId = "");

// 统一推送入口：分布式走 Redis，单机直接写连接
void publishToUser(const std::string& userId, const std::string& data,
                   const std::string& connId = "");
```

### ChatSseHandler 变更

`onToken` / `onDone` / `onError` / `onToolCall` 回调统一改为调用 `publishToUser`，
不再直接持有 `sseConn` 写数据（`sseConn` 仅用于发送 SSE 握手前的 meta 事件）。

---

## 部署方式

### 单机部署（不变）

注释掉 `chat_main.cpp` 中的 `initRedis` 调用：

```cpp
// http::sse::SseManager::instance().initRedis(redisUri);
```

此时 `publishToUser` 退化为直接写本地连接，行为与改造前完全一致。

### 多实例部署

**1. 启动 Redis**（已有则跳过）

```bash
redis-server
```

**2. 编译并启动多个实例**

```bash
# 实例 1
REDIS_URI=tcp://127.0.0.1:6379 DB_HOST=... ./chat_server 8080

# 实例 2
REDIS_URI=tcp://127.0.0.1:6379 DB_HOST=... ./chat_server 8081
```

**3. Nginx 负载均衡**

```nginx
upstream chat_backend {
    ip_hash;                    # 同一 IP 路由到同一实例（保证 SSE 连接稳定）
    server 127.0.0.1:8080;
    server 127.0.0.1:8081;
}

server {
    listen 80;
    location / {
        proxy_pass http://chat_backend;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_buffering off;            # SSE 必须关闭缓冲
        proxy_read_timeout 300s;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

> `ip_hash` 确保同一用户的 SSE 连接和后续 POST 请求落在同一实例，
> 即使不用 `ip_hash`，Redis Pub/Sub 也能保证跨实例推送正确。
> `ip_hash` 只是减少不必要的跨实例通信。

**4. Redis 高可用（可选）**

```bash
# Sentinel 模式
REDIS_URI=redis://sentinel1:26379,sentinel2:26379/mymaster ./chat_server 8080

# Cluster 模式
REDIS_URI=redis://node1:7000,node2:7001,node3:7002 ./chat_server 8080
```

`redis++` 原生支持 Sentinel 和 Cluster，`RedisPubSub` 无需修改。

---

## 数据流（分布式模式）

```
用户 POST /api/chat/stream
    │
    ▼
实例 A：ChatSseHandler::handle()
    ├─ SSE 握手，addConnection(conn, userId="42")
    │      └─ SseManager 订阅 "sse:user:42"
    ├─ 发送 meta 事件（conversation_id, model）
    └─ MCPAgent::chat() 异步执行
           │
           ├─ onToken(token)
           │      └─ publishToUser("42", token)
           │              └─ redis.publish("sse:user:42", token)
           │                      └─ 所有订阅该 channel 的实例收到
           │                              └─ 写入 TCP 连接 → 浏览器
           │
           └─ onDone()
                  ├─ 入库（MySQL）
                  ├─ publishToUser("42", "[DONE]")
                  └─ removeConnection → unsubscribe("sse:user:42")
```

---

## 兼容性

- 未登录用户（`userId == 0`）：`publishToUser` 退化为直接写本地连接，行为不变
- 单机模式（未调用 `initRedis`）：`pubsub_` 为 null，`publishToUser` 直接写本地连接
- 现有 API（`/api/auth/*`, `/api/conversations/*` 等）无任何变更
