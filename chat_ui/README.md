# Chat UI

基于 [HTTPServer](../HttpServer) 框架构建的多模型 AI 聊天服务，支持 SSE 流式输出、MCP 工具调用、用户认证、会话持久化，以及 Redis Pub/Sub 分布式部署。

## 目录结构

```
chat_ui/
├── chat_main.cpp          # 入口：初始化、路由注册、服务启动
├── chat_ui.html           # 单页前端
├── config.json            # 多模型配置
├── CMakeLists.txt
├── sql/init.sql           # 数据库 Schema
│
├── ai/                    # 多模型抽象层
│   ├── AIStrategy.h       # 抽象接口
│   ├── AIFactory.h        # 注册式工厂（单例）
│   ├── AIConfig.h         # 加载 config.json
│   ├── AIStrategies.h     # 各厂商策略实现
│   └── ModelRegister.h    # 静态自动注册触发器
│
├── sse/                   # SSE 流式推送
│   ├── ChatSseHandler.h   # POST /api/chat/stream 处理器
│   ├── SseManager.h       # 连接管理 + Redis 跨实例转发
│   └── RedisPubSub.h      # Redis Pub/Sub 封装
│
├── mcp/                   # 工具调用（MCP 协议）
│   ├── MCPAgent.h         # Agentic loop（最多 3 轮工具调用）
│   ├── MCPTool.h          # 工具抽象
│   ├── MCPToolRegistry.h  # 工具注册表
│   └── BuiltinTools.h     # 内置工具实现
│
├── auth/                  # 用户认证
│   ├── AuthHandler.h      # 注册 / 登录 / 登出
│   ├── AuthMiddleware.h   # Session 校验
│   └── PasswordUtil.h     # 密码哈希（SHA256 + salt）
│
├── api/                   # REST 接口
│   ├── ConversationHandler.h  # 会话 CRUD
│   └── MessageHandler.h       # 消息列表
│
└── dao/                   # 数据访问层
    ├── UserDao.h
    ├── ConversationDao.h
    └── MessageDao.h
```

## 架构概览

```
Browser
  │  GET /          → chat_ui.html
  │  POST /api/auth/*
  │  GET/POST /api/conversations/*
  └  POST /api/chat/stream  ──────────────────────────────┐
                                                           │
┌──────────────────────────────────────────────────────────▼──┐
│                        ChatSseHandler                        │
│  1. 鉴权（Session）                                          │
│  2. 解析消息，创建/关联 Conversation                         │
│  3. SSE 握手，注册 SseManager                                │
│  4. AIFactory 选模型 → AIStrategy                            │
│  5. MCPAgent.chat()  ←→  MCP 工具调用（最多 3 轮）           │
│  6. onToken → publishToUser → 浏览器收到流式 token           │
└──────────────────────────────────────────────────────────────┘
         │                          │
    SseManager                 AIFactory
    (+ RedisPubSub)         (策略模式注册表)
         │                          │
    Redis Pub/Sub          Claude / Qwen / Doubao / Wenxin
    跨实例 SSE 转发         (OpenAI 兼容接口)
         │
    MySQL（消息持久化）
```

## 核心模块

### 多模型工厂（ai/）

策略模式 + 注册式工厂，新增模型只需实现 `AIStrategy` 并调用 `registerModel`。

```
AIFactory（单例）
  ├── "claude"  → ClaudeStrategy
  ├── "qwen"    → QwenStrategy
  ├── "doubao"  → DoubaoStrategy
  └── "wenxin"  → WenxinStrategy
```

所有厂商均使用 OpenAI 兼容接口（`/v1/chat/completions`），通过 `config.json` 配置 `base_url` 和 `api_key`。

### SSE 流式推送（sse/）

```
ChatSseHandler
  └── MCPAgent.chat(onToken, onDone, onError, onToolCall)
            │
            └── SseManager::publishToUser(userId, token)
                      ├── 分布式模式：redis.publish("sse:user:{id}", token)
                      │         └── 订阅该 channel 的实例写入 TCP 连接
                      └── 单机模式：直接写 TcpConnectionPtr
```

### MCP 工具调用（mcp/）

`MCPAgent` 实现 agentic loop：LLM 返回 tool_call → 执行工具 → 结果追加到上下文 → 再次调用 LLM，最多循环 3 轮。工具通过 `MCPToolRegistry` 注册，`BuiltinTools` 提供内置实现。

### 数据库 Schema

| 表 | 说明 |
|---|---|
| `users` | 用户名 + 密码哈希 + salt |
| `conversations` | 会话（归属 user_id，含 title） |
| `messages` | 消息（role: user/assistant/system，content） |

## 配置

### config.json

```json
{
  "default_model": "claude",
  "models": {
    "claude":  { "api_key": "...", "base_url": "...", "model": "claude-sonnet-4-6" },
    "qwen":    { "api_key": "...", "base_url": "https://dashscope.aliyuncs.com", "model": "qwen-plus" },
    "doubao":  { "api_key": "...", "base_url": "https://ark.cn-beijing.volces.com", "model": "ep-xxx" },
    "wenxin":  { "api_key": "...", "base_url": "https://aip.baidubce.com", "model": "ernie-speed-128k" }
  }
}
```

找不到 `config.json` 时，从环境变量读取：

| 变量 | 说明 |
|---|---|
| `ANTHROPIC_AUTH_TOKEN` | API Key |
| `ANTHROPIC_BASE_URL` | API Base URL |
| `ANTHROPIC_MODEL` | 模型名 |

### 其他环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `REDIS_URI` | `tcp://127.0.0.1:6379` | Redis 地址 |
| `DB_HOST` | `localhost` | MySQL 主机 |
| `DB_USER` | `root` | MySQL 用户 |
| `DB_PASS` | `123456` | MySQL 密码 |
| `DB_NAME` | `chat_app` | 数据库名 |
| `DB_POOL_SIZE` | `10` | 连接池大小 |

## 构建与运行

```bash
# 初始化数据库
mysql -u root -p < sql/init.sql

# 构建
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# 运行（单机）
REDIS_URI=tcp://127.0.0.1:6379 ./chat_server 8080
```

访问 `http://localhost:8080`

## API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 前端页面 |
| GET | `/api/health` | 健康检查，返回可用模型列表 |
| POST | `/api/auth/register` | 注册 |
| POST | `/api/auth/login` | 登录 |
| POST | `/api/auth/logout` | 登出 |
| GET | `/api/auth/me` | 当前用户信息 |
| GET | `/api/conversations` | 会话列表 |
| POST | `/api/conversations` | 创建会话 |
| PUT | `/api/conversations/:id` | 重命名会话 |
| DELETE | `/api/conversations/:id` | 删除会话 |
| GET | `/api/conversations/:id/messages` | 消息列表 |
| POST | `/api/chat/stream` | SSE 流式聊天 |

### POST /api/chat/stream

请求体：
```json
{
  "model": "claude",
  "conversation_id": 1,
  "messages": [
    { "role": "user", "content": "你好" }
  ]
}
```

SSE 事件流：
```
event: meta
data: {"conversation_id":1}

event: meta
data: {"model":"claude-sonnet-4-6","provider":"claude"}

data: {"token":"你"}
data: {"token":"好"}

event: tool
data: {"tool":"search","result":"..."}

data: [DONE]
```

## 分布式部署

见 [DISTRIBUTED.md](./DISTRIBUTED.md)。核心机制：每个实例订阅 `sse:user:{userId}` Redis channel，AI Worker 通过 `redis.publish` 广播 token，持有该用户 SSE 连接的实例负责写入 TCP。

```
实例 A（用户连接）          实例 B（AI 处理）
SseManager                 ChatSseHandler
  subscribe("sse:user:42") ←── publish("sse:user:42", token)
  → 写入 TCP 连接
```

Nginx 配置使用 `ip_hash` 保持连接稳定，Redis 支持 Sentinel / Cluster 高可用模式。
