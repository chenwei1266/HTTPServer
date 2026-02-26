#pragma once

#include <functional>
#include <string>
#include <thread>
#include <memory>
#include <sstream>
#include <curl/curl.h>

#include <muduo/base/Logging.h>

namespace llm
{

struct LlmConfig
{
    std::string baseUrl = "http://localhost:11434"; // Ollama 默认地址
    std::string apiKey  = "";                       // API Key
    std::string model   = "qwen2.5:7b";            // 默认模型
    int timeout         = 120;                      // 秒
    bool isOpenAI       = false;                    // true=OpenAI兼容, false=Ollama
    int maxTokens       = 4096;                     // max_tokens (Claude 必需)
};

class LlmClient
{
public:
    using TokenCallback = std::function<void(const std::string& token)>;
    using DoneCallback  = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    struct Message
    {
        std::string role;    // "system" | "user" | "assistant"
        std::string content;
    };

    explicit LlmClient(const LlmConfig& config = LlmConfig{})
        : config_(config)
    {}

    void streamChat(
        const std::vector<Message>& messages,
        TokenCallback onToken,
        DoneCallback  onDone,
        ErrorCallback onError)
    {
        std::thread([this, messages, onToken, onDone, onError]() {
            doStreamChat(messages, onToken, onDone, onError);
        }).detach();
    }

private:
    struct CurlContext
    {
        TokenCallback onToken;
        ErrorCallback onError;
        bool isOpenAI;
        std::string buffer;
    };

    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        size_t totalSize = size * nmemb;
        auto* ctx = static_cast<CurlContext*>(userdata);

        ctx->buffer.append(ptr, totalSize);

        std::string& buf = ctx->buffer;
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos)
        {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty()) continue;

            if (ctx->isOpenAI)
            {
                if (line.rfind("data: ", 0) == 0)
                {
                    std::string json = line.substr(6);
                    if (json == "[DONE]") continue;
                    std::string token = extractOpenAIToken(json);
                    if (!token.empty()) ctx->onToken(token);
                }
            }
            else
            {
                std::string token = extractOllamaToken(line);
                if (!token.empty()) ctx->onToken(token);
            }
        }

        return totalSize;
    }

    // OpenAI 兼容格式的 token 提取
    // 流式响应中 delta 格式: {"choices":[{"delta":{"content":"xxx"}}]}
    static std::string extractOpenAIToken(const std::string& json)
    {
        // 先尝试 delta.content 格式（流式）
        // 查找 "delta" 然后找其中的 "content"
        auto deltaPos = json.find("\"delta\"");
        if (deltaPos != std::string::npos)
        {
            // 从 delta 位置开始找 content
            auto contentPos = json.find("\"content\"", deltaPos);
            if (contentPos != std::string::npos)
            {
                contentPos += 9; // strlen("\"content\"")
                // 跳过 : 和空格
                while (contentPos < json.size() && (json[contentPos] == ':' || json[contentPos] == ' '))
                    ++contentPos;

                if (contentPos < json.size())
                {
                    if (json[contentPos] == '"')
                    {
                        ++contentPos;
                        return extractQuotedString(json, contentPos);
                    }
                    // content 可能是 null
                    if (json.compare(contentPos, 4, "null") == 0)
                        return "";
                }
            }
        }

        // 回退：直接找 "content":"..."
        auto pos = json.find("\"content\":\"");
        if (pos == std::string::npos) return "";
        pos += 11;
        return extractQuotedString(json, pos);
    }

    // 从指定位置提取引号内的字符串（处理转义）
    static std::string extractQuotedString(const std::string& json, size_t pos)
    {
        std::string result;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                ++pos;
                switch (json[pos]) {
                    case 'n':  result += '\n'; break;
                    case 't':  result += '\t'; break;
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case 'r':  result += '\r'; break;
                    case '/':  result += '/';  break;
                    case 'u': {
                        // 简单处理 \uXXXX unicode 转义
                        if (pos + 4 < json.size())
                        {
                            std::string hex = json.substr(pos + 1, 4);
                            unsigned int codepoint = 0;
                            std::istringstream iss(hex);
                            iss >> std::hex >> codepoint;
                            if (codepoint < 0x80) {
                                result += static_cast<char>(codepoint);
                            } else if (codepoint < 0x800) {
                                result += static_cast<char>(0xC0 | (codepoint >> 6));
                                result += static_cast<char>(0x80 | (codepoint & 0x3F));
                            } else {
                                result += static_cast<char>(0xE0 | (codepoint >> 12));
                                result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                                result += static_cast<char>(0x80 | (codepoint & 0x3F));
                            }
                            pos += 4;
                        }
                        break;
                    }
                    default: result += json[pos]; break;
                }
            }
            else
            {
                result += json[pos];
            }
            ++pos;
        }
        return result;
    }

    static std::string extractOllamaToken(const std::string& json)
    {
        auto tryExtract = [&](const std::string& key) -> std::string {
            auto pos = json.find(key);
            if (pos == std::string::npos) return "";
            pos += key.size();
            if (pos >= json.size() || json[pos] != '"') return "";
            ++pos;
            return extractQuotedString(json, pos);
        };

        std::string token = tryExtract("\"content\":\"");
        if (!token.empty()) return token;
        return tryExtract("\"response\":\"");
    }

    void doStreamChat(
        const std::vector<Message>& messages,
        TokenCallback onToken,
        DoneCallback  onDone,
        ErrorCallback onError)
    {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            onError("curl_easy_init failed");
            return;
        }

        std::string body = buildRequestBody(messages);
        std::string url  = buildUrl();

        LOG_INFO << "LLM request to: " << url;
        LOG_INFO << "LLM request body: " << body;

        CurlContext ctx;
        ctx.onToken  = onToken;
        ctx.onError  = onError;
        ctx.isOpenAI = config_.isOpenAI;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!config_.apiKey.empty())
        {
            std::string auth = "Authorization: Bearer " + config_.apiKey;
            headers = curl_slist_append(headers, auth.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        // 对 HTTPS 的支持
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            onError(std::string("curl error: ") + curl_easy_strerror(res));
            return;
        }

        if (httpCode != 200)
        {
            // 缓冲区里可能还有未处理的错误信息
            if (!ctx.buffer.empty())
            {
                onError("HTTP " + std::to_string(httpCode) + ": " + ctx.buffer);
                return;
            }
            onError("HTTP error: " + std::to_string(httpCode));
            return;
        }

        onDone();
    }

    std::string buildUrl() const
    {
        if (config_.isOpenAI)
            return config_.baseUrl + "/v1/chat/completions";
        else
            return config_.baseUrl + "/api/chat";
    }

    std::string buildRequestBody(const std::vector<Message>& messages) const
    {
        std::ostringstream oss;
        oss << "{";
        oss << "\"model\":\"" << config_.model << "\",";
        oss << "\"stream\":true,";

        // Claude 模型必须指定 max_tokens
        if (config_.isOpenAI && config_.maxTokens > 0)
        {
            oss << "\"max_tokens\":" << config_.maxTokens << ",";
        }

        oss << "\"messages\":[";
        for (size_t i = 0; i < messages.size(); ++i)
        {
            if (i > 0) oss << ",";
            oss << "{\"role\":\"" << messages[i].role << "\","
                << "\"content\":\"" << escapeJson(messages[i].content) << "\"}";
        }
        oss << "]}";
        return oss.str();
    }

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

private:
    LlmConfig config_;
};

} // namespace llm
