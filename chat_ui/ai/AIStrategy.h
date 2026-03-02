#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace ai
{

// ─── 消息结构 ────────────────────────────────────────────────
struct Message
{
    std::string role;    // "system" | "user" | "assistant"
    std::string content;
};

// ─── 模型配置（每个 Strategy 自己填充默认值）────────────────
struct ModelConfig
{
    std::string apiKey;
    std::string baseUrl;
    std::string model;
    int         maxTokens = 4096;
    int         timeout   = 120;
};

// ─── 回调类型 ────────────────────────────────────────────────
using TokenCallback = std::function<void(const std::string& token)>;
using DoneCallback  = std::function<void()>;
using ErrorCallback = std::function<void(const std::string& error)>;

// ─── 同步返回结果 ─────────────────────────────────────────────
struct ChatResult
{
    bool        success = false;
    std::string content;
    std::string error;
    int         inputTokens  = 0;
    int         outputTokens = 0;
};

// ════════════════════════════════════════════════════════════
//  AIStrategy  —— 所有厂商实现类必须继承此接口
// ════════════════════════════════════════════════════════════
class AIStrategy
{
public:
    explicit AIStrategy(const ModelConfig& config)
        : config_(config) {}

    virtual ~AIStrategy() = default;

    // 禁止拷贝
    AIStrategy(const AIStrategy&)            = delete;
    AIStrategy& operator=(const AIStrategy&) = delete;

    // ── 纯虚接口 ──────────────────────────────────────────

    // 同步对话（适合工具调用结果回填等短请求）
    virtual ChatResult sendMessage(const std::vector<Message>& messages) = 0;

    // 流式对话（SSE 场景）
    virtual void sendStreamMsg(
        const std::vector<Message>& messages,
        TokenCallback onToken,
        DoneCallback  onDone,
        ErrorCallback onError) = 0;

    // 返回当前使用的模型名称（用于日志、前端展示）
    virtual std::string getModelName() const = 0;

    // 返回厂商名称（用于工厂注册 key）
    virtual std::string getProviderName() const = 0;

protected:
    ModelConfig config_;

    // ── 子类公用的 JSON 工具 ──────────────────────────────
    static std::string escapeJson(const std::string& s)
    {
        std::string result;
        result.reserve(s.size());
        for (char c : s)
        {
            switch (c)
            {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n";  break;
                case '\r': result += "\\r";  break;
                case '\t': result += "\\t";  break;
                default:   result += c;      break;
            }
        }
        return result;
    }

    // 将 messages 序列化为 JSON 数组字符串
    static std::string buildMessagesJson(const std::vector<Message>& messages)
    {
        std::string json = "[";
        for (size_t i = 0; i < messages.size(); ++i)
        {
            if (i > 0) json += ",";
            json += "{\"role\":\"" + messages[i].role + "\","
                  + "\"content\":\"" + escapeJson(messages[i].content) + "\"}";
        }
        json += "]";
        return json;
    }
};

} // namespace ai
