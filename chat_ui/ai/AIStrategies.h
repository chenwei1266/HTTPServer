#pragma once

// ════════════════════════════════════════════════════════════
//  各厂商 Strategy 实现
//  每个类只需关心：
//    1. 自己的默认 baseUrl / model
//    2. 请求体差异（如有）
//    3. 响应解析差异（如有）
// ════════════════════════════════════════════════════════════

#include "OpenAICompatibleStrategy.h"
#include <sstream>

namespace ai
{

// ─────────────────────────────────────────────────────────────
//  字节跳动·豆包 (火山引擎 OpenAI 兼容接口)
//  文档: https://www.volcengine.com/docs/82379/1302069
// ─────────────────────────────────────────────────────────────
class DoubaoStrategy : public OpenAICompatibleStrategy
{
public:
    explicit DoubaoStrategy(const ModelConfig& config)
        : OpenAICompatibleStrategy(fillDefaults(config)) {}

    std::string getModelName()    const override { return config_.model; }
    std::string getProviderName() const override { return "doubao"; }

protected:
    // 豆包使用火山引擎接入点，鉴权头与 OpenAI 相同
    std::string buildUrl() const override
    {
        return config_.baseUrl + "/api/v3/chat/completions";
    }

private:
    static ModelConfig fillDefaults(ModelConfig cfg)
    {
        if (cfg.baseUrl.empty()) cfg.baseUrl = "https://ark.cn-beijing.volces.com";
        // 豆包模型名称是 endpoint_id，格式: ep-xxxxxx-yyyy
        // 这里填写默认，用户可通过 config.json 覆盖
        if (cfg.model.empty())  cfg.model   = "ep-20250101000000-xxxxx";
        return cfg;
    }
};


// ─────────────────────────────────────────────────────────────
//  阿里云·通义千问 (DashScope OpenAI 兼容接口)
//  文档: https://help.aliyun.com/zh/model-studio/openai-compatibility
// ─────────────────────────────────────────────────────────────
class QwenStrategy : public OpenAICompatibleStrategy
{
public:
    explicit QwenStrategy(const ModelConfig& config)
        : OpenAICompatibleStrategy(fillDefaults(config)) {}

    std::string getModelName()    const override { return config_.model; }
    std::string getProviderName() const override { return "qwen"; }

protected:
    std::string buildUrl() const override
    {
        return config_.baseUrl + "/compatible-mode/v1/chat/completions";
    }

    // 通义支持 incremental_output 参数，流式体验更好
    std::string buildRequestBody(
        const std::vector<Message>& messages, bool stream) const override
    {
        std::ostringstream oss;
        oss << "{"
            << "\"model\":\""          << config_.model    << "\","
            << "\"stream\":"           << (stream ? "true" : "false") << ","
            << "\"max_tokens\":"       << config_.maxTokens << ","
            << "\"stream_options\":"   << (stream ? "{\"include_usage\":false}" : "null") << ","
            << "\"messages\":"         << buildMessagesJson(messages)
            << "}";
        return oss.str();
    }

private:
    static ModelConfig fillDefaults(ModelConfig cfg)
    {
        if (cfg.baseUrl.empty()) cfg.baseUrl = "https://dashscope.aliyuncs.com";
        if (cfg.model.empty())   cfg.model   = "qwen-plus";
        return cfg;
    }
};


// ─────────────────────────────────────────────────────────────
//  百度·文心一言 (ERNIE Speed / Pro)
//  百度的接口与 OpenAI 不完全兼容，需要单独处理：
//    - 鉴权用 access_token 放在 URL 参数而非 Header
//    - 响应体格式不同：result 字段而非 choices[0].message.content
//    - 流式格式: data: {"result":"xxx","is_end":false}
// ─────────────────────────────────────────────────────────────
class WenxinStrategy : public AIStrategy
{
public:
    explicit WenxinStrategy(const ModelConfig& config)
        : AIStrategy(fillDefaults(config)) {}

    std::string getModelName()    const override { return config_.model; }
    std::string getProviderName() const override { return "wenxin"; }

    ChatResult sendMessage(const std::vector<Message>& messages) override
    {
        ChatResult result;
        std::string body = buildRequestBody(messages, false);
        std::string url  = buildUrl();

        // 百度用 access_token 放 URL
        url += "?access_token=" + config_.apiKey;

        std::string responseBody;
        long code = doPost(url, body, responseBody);

        if (code != 200)
        {
            result.error = "HTTP " + std::to_string(code) + ": " + responseBody;
            return result;
        }

        // 百度同步响应: {"result":"xxx",...}
        auto pos = responseBody.find("\"result\":\"");
        if (pos != std::string::npos)
        {
            result.content = extractQS(responseBody, pos + 10);  // 使用类内的 extractQS
            result.success = true;
        }
        else
        {
            result.error = "unexpected response: " + responseBody;
        }
        return result;
    }

    void sendStreamMsg(
        const std::vector<Message>& messages,
        TokenCallback onToken,
        DoneCallback  onDone,
        ErrorCallback onError) override
    {
        std::thread([this, messages, onToken, onDone, onError]() {
            doStream(messages, onToken, onDone, onError);
        }).detach();
    }

private:
    // 百度模型名到 URL 路径的映射
    std::string buildUrl() const
    {
        static const std::unordered_map<std::string, std::string> pathMap = {
            {"ernie-speed-128k",    "/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/ernie-speed-128k"},
            {"ernie-4.0-turbo-8k",  "/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/ernie-4.0-turbo-8k"},
            {"ernie-3.5-8k",        "/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/ernie-3.5-8k"},
        };
        auto it = pathMap.find(config_.model);
        std::string path = (it != pathMap.end())
            ? it->second
            : "/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/ernie-speed-128k";
        return config_.baseUrl + path;
    }

    std::string buildRequestBody(
        const std::vector<Message>& messages, bool stream) const
    {
        // 百度不支持 system 消息放在 messages 里（需要单独 system 字段）
        std::string systemPrompt;
        std::vector<Message> filtered;
        for (auto& m : messages)
        {
            if (m.role == "system") systemPrompt = m.content;
            else filtered.push_back(m);
        }

        std::ostringstream oss;
        oss << "{"
            << "\"messages\":" << buildMessagesJson(filtered) << ","
            << "\"stream\":"   << (stream ? "true" : "false");
        if (!systemPrompt.empty())
            oss << ",\"system\":\"" << escapeJson(systemPrompt) << "\"";
        oss << "}";
        return oss.str();
    }

    void doStream(
        const std::vector<Message>& messages,
        TokenCallback onToken,
        DoneCallback  onDone,
        ErrorCallback onError)
    {
        CURL* curl = curl_easy_init();
        if (!curl) { onError("curl_easy_init failed"); return; }

        std::string body = buildRequestBody(messages, true);
        std::string url  = buildUrl() + "?access_token=" + config_.apiKey;

        struct WenxinCtx {
            TokenCallback onToken;
            std::string   buffer;
        } ctx{ onToken, "" };

        auto writeCb = [](char* ptr, size_t sz, size_t nm, void* ud) -> size_t {
            size_t total = sz * nm;
            auto* c = static_cast<WenxinCtx*>(ud);
            c->buffer.append(ptr, total);

            size_t pos;
            while ((pos = c->buffer.find('\n')) != std::string::npos)
            {
                std::string line = c->buffer.substr(0, pos);
                c->buffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data: ", 0) != 0) continue;

                std::string data = line.substr(6);
                // 百度流式: {"result":"xxx","is_end":false}
                auto rpos = data.find("\"result\":\"");
                if (rpos != std::string::npos)
                {
                    std::string token = extractQS(data, rpos + 10);
                    if (!token.empty()) c->onToken(token);
                }
            }
            return total;
        };

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         static_cast<size_t(*)(char*,size_t,size_t,void*)>(writeCb));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,       (long)config_.timeout);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) { onError(curl_easy_strerror(res)); return; }
        if (httpCode != 200) { onError("HTTP " + std::to_string(httpCode)); return; }
        onDone();
    }

    long doPost(const std::string& url, const std::string& body, std::string& out)
    {
        CURL* curl = curl_easy_init();
        if (!curl) return -1;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        auto writeCb = [](char* p, size_t s, size_t n, void* ud) -> size_t {
            static_cast<std::string*>(ud)->append(p, s * n);
            return s * n;
        };

        curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         static_cast<size_t(*)(char*,size_t,size_t,void*)>(writeCb));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &out);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,       (long)config_.timeout);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);

        curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return code;
    }

    static std::string extractQS(const std::string& s, size_t pos)
    {
        std::string result;
        while (pos < s.size() && s[pos] != '"')
        {
            if (s[pos] == '\\' && pos + 1 < s.size())
            {
                ++pos;
                switch (s[pos]) {
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case '"': result += '"';  break;
                    default:  result += s[pos]; break;
                }
            }
            else result += s[pos];
            ++pos;
        }
        return result;
    }

    static ModelConfig fillDefaults(ModelConfig cfg)
    {
        if (cfg.baseUrl.empty()) cfg.baseUrl = "https://aip.baidubce.com";
        if (cfg.model.empty())   cfg.model   = "ernie-speed-128k";
        return cfg;
    }
};

// ─────────────────────────────────────────────────────────────
//  Anthropic·Claude (OpenAI 兼容接口 / 自建中转均可)
//  与原 LlmClient(isOpenAI=true) 行为完全一致：
//    - Authorization: Bearer <apiKey>
//    - POST /v1/chat/completions
//    - stream: true + max_tokens
// ─────────────────────────────────────────────────────────────
class ClaudeStrategy : public OpenAICompatibleStrategy
{
public:
    explicit ClaudeStrategy(const ModelConfig& config)
        : OpenAICompatibleStrategy(fillDefaults(config)) {}

    std::string getModelName()    const override { return config_.model; }
    std::string getProviderName() const override { return "claude"; }

    // buildUrl / buildHeaders / extractToken 全部继承自
    // OpenAICompatibleStrategy，与原 LlmClient 行为完全相同，无需覆写

private:
    static ModelConfig fillDefaults(ModelConfig cfg)
    {
        // 默认指向官方接口；使用中转站时在 config.json 里覆盖 base_url
        if (cfg.baseUrl.empty()) cfg.baseUrl = "https://api.anthropic.com";
        if (cfg.model.empty())   cfg.model   = "claude-sonnet-4-5-20250929";
        return cfg;
    }
};

} // namespace ai