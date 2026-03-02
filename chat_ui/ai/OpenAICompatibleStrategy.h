#pragma once

#include "AIStrategy.h"
#include <thread>
#include <sstream>
#include <curl/curl.h>

namespace ai
{

// ════════════════════════════════════════════════════════════
//  OpenAICompatibleStrategy
//  继承此类的厂商只需覆写：
//    - buildRequestBody()   构造请求体
//    - extractToken()       从 SSE data 行提取 token
//    - getModelName()
//    - getProviderName()
//  不需要关心 curl 细节、流式分包、线程管理
// ════════════════════════════════════════════════════════════
class OpenAICompatibleStrategy : public AIStrategy
{
public:
    explicit OpenAICompatibleStrategy(const ModelConfig& config)
        : AIStrategy(config) {}

    // ── 同步对话 ──────────────────────────────────────────
    ChatResult sendMessage(const std::vector<Message>& messages) override
    {
        ChatResult result;
        std::string responseBody;

        auto code = doPost(buildUrl(), buildRequestBody(messages, false),
                           buildHeaders(),
                           [&responseBody](const char* data, size_t size) {
                               responseBody.append(data, size);
                           });

        if (code != 200)
        {
            result.error = "HTTP " + std::to_string(code) + ": " + responseBody;
            return result;
        }

        result.content = extractSyncContent(responseBody);
        result.success = !result.content.empty();
        return result;
    }

    // ── 流式对话 ──────────────────────────────────────────
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

protected:
    // ── 子类可覆写的钩子 ──────────────────────────────────

    virtual std::string buildUrl() const
    {
        return config_.baseUrl + "/v1/chat/completions";
    }

    virtual std::string buildRequestBody(
        const std::vector<Message>& messages, bool stream) const
    {
        std::ostringstream oss;
        oss << "{"
            << "\"model\":\""    << config_.model    << "\","
            << "\"stream\":"     << (stream ? "true" : "false") << ","
            << "\"max_tokens\":" << config_.maxTokens << ","
            << "\"messages\":"   << buildMessagesJson(messages)
            << "}";
        return oss.str();
    }

    virtual std::vector<std::string> buildHeaders() const
    {
        return {
            "Content-Type: application/json",
            "Authorization: Bearer " + config_.apiKey
        };
    }

    // 从流式 data: {...} 行提取 token（子类按需覆写）
    virtual std::string extractToken(const std::string& dataLine) const
    {
        // 通用 OpenAI 格式: choices[0].delta.content
        auto deltaPos = dataLine.find("\"delta\"");
        if (deltaPos == std::string::npos) return "";

        auto contentPos = dataLine.find("\"content\"", deltaPos);
        if (contentPos == std::string::npos) return "";

        contentPos += 9;
        while (contentPos < dataLine.size() &&
               (dataLine[contentPos] == ':' || dataLine[contentPos] == ' '))
            ++contentPos;

        if (contentPos >= dataLine.size() || dataLine[contentPos] != '"')
            return "";

        return extractQuotedString(dataLine, contentPos + 1);
    }

    // 从同步响应体中提取 content
    virtual std::string extractSyncContent(const std::string& body) const
    {
        auto pos = body.find("\"content\":\"");
        if (pos == std::string::npos) return "";
        return extractQuotedString(body, pos + 11);
    }

private:
    // ── curl 实现细节（子类不需要关心）────────────────────

    struct StreamCtx
    {
        TokenCallback onToken;
        ErrorCallback onError;
        std::string   buffer;
        const OpenAICompatibleStrategy* self;
    };

    static size_t streamWriteCb(char* ptr, size_t size, size_t nmemb, void* ud)
    {
        size_t total = size * nmemb;
        auto* ctx = static_cast<StreamCtx*>(ud);
        ctx->buffer.append(ptr, total);

        size_t pos;
        while ((pos = ctx->buffer.find('\n')) != std::string::npos)
        {
            std::string line = ctx->buffer.substr(0, pos);
            ctx->buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (line.rfind("data: ", 0) == 0)
            {
                std::string data = line.substr(6);
                if (data == "[DONE]") continue;
                std::string token = ctx->self->extractToken(data);
                if (!token.empty()) ctx->onToken(token);
            }
        }
        return total;
    }

    static size_t syncWriteCb(char* ptr, size_t size, size_t nmemb, void* ud)
    {
        auto* buf = static_cast<std::string*>(ud);
        buf->append(ptr, size * nmemb);
        return size * nmemb;
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
        std::string url  = buildUrl();
        auto rawHeaders  = buildHeaders();

        struct curl_slist* headers = nullptr;
        for (auto& h : rawHeaders)
            headers = curl_slist_append(headers, h.c_str());

        StreamCtx ctx{ onToken, onError, "", this };

        curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,       (long)config_.timeout);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            onError(std::string("curl: ") + curl_easy_strerror(res));
            return;
        }
        if (httpCode != 200)
        {
            onError("HTTP " + std::to_string(httpCode) +
                    (ctx.buffer.empty() ? "" : ": " + ctx.buffer));
            return;
        }
        onDone();
    }

    long doPost(const std::string& url,
                const std::string& body,
                const std::vector<std::string>& rawHeaders,
                std::function<void(const char*, size_t)> writer)
    {
        CURL* curl = curl_easy_init();
        if (!curl) return -1;

        struct curl_slist* headers = nullptr;
        for (auto& h : rawHeaders)
            headers = curl_slist_append(headers, h.c_str());

        std::string buf;
        curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, syncWriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,       (long)config_.timeout);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);

        curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        writer(buf.c_str(), buf.size());
        return code;
    }

    static std::string extractQuotedString(const std::string& s, size_t pos)
    {
        std::string result;
        while (pos < s.size() && s[pos] != '"')
        {
            if (s[pos] == '\\' && pos + 1 < s.size())
            {
                ++pos;
                switch (s[pos]) {
                    case 'n':  result += '\n'; break;
                    case 't':  result += '\t'; break;
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case 'r':  result += '\r'; break;
                    default:   result += s[pos]; break;
                }
            }
            else result += s[pos];
            ++pos;
        }
        return result;
    }
};

} // namespace ai
