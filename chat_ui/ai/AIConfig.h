#pragma once

#include "AIStrategy.h"
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iostream>

namespace ai
{

// ════════════════════════════════════════════════════════════
//  AIConfig  —— 从 config.json 加载各厂商配置
//
//  config.json 格式：
//  {
//    "default_model": "qwen",
//    "models": {
//      "qwen": {
//        "api_key":    "sk-xxx",
//        "base_url":   "https://dashscope.aliyuncs.com",
//        "model":      "qwen-plus",
//        "max_tokens": 4096,
//        "timeout":    120
//      },
//      "doubao": { ... },
//      "wenxin": { ... }
//    }
//  }
// ════════════════════════════════════════════════════════════

class AIConfig
{
public:
    bool load(const std::string& path)
    {
        std::ifstream ifs(path);
        if (!ifs.is_open()) return false;

        std::ostringstream oss;
        oss << ifs.rdbuf();
        raw_ = oss.str();

        defaultModel_ = extractString(raw_, "default_model");

        auto modelsStart = raw_.find("\"models\"");
        if (modelsStart == std::string::npos) return true;

        modelsStart = raw_.find('{', modelsStart + 8);
        if (modelsStart == std::string::npos) return true;

        size_t pos = modelsStart + 1;
        while (pos < raw_.size())
        {
            while (pos < raw_.size() && std::isspace((unsigned char)raw_[pos])) ++pos;
            if (pos >= raw_.size() || raw_[pos] == '}') break;
            if (raw_[pos] != '"') { ++pos; continue; }

            ++pos;
            std::string key;
            while (pos < raw_.size() && raw_[pos] != '"') key += raw_[pos++];
            ++pos;

            while (pos < raw_.size() && raw_[pos] != '{') ++pos;
            size_t objEnd = findObjectEnd(raw_, pos);
            std::string obj = raw_.substr(pos, objEnd - pos + 1);

            ModelConfig cfg;
            cfg.apiKey    = extractString(obj, "api_key");
            cfg.baseUrl   = extractString(obj, "base_url");
            cfg.model     = extractString(obj, "model");
            cfg.maxTokens = extractInt(obj, "max_tokens", 4096);
            cfg.timeout   = extractInt(obj, "timeout",    120);

            while (!cfg.baseUrl.empty() && cfg.baseUrl.back() == '/')
                cfg.baseUrl.pop_back();

            configs_[key] = cfg;
            pos = objEnd + 1;
        }

        std::cout << "[AIConfig] Loaded " << configs_.size()
                  << " model config(s). Default: \"" << defaultModel_ << "\"\n";
        return true;
    }

    // config.json 不存在时，允许外部设置 fallback 默认模型名
    void setFallbackModel(const std::string& modelKey)
    {
        if (defaultModel_.empty())
            defaultModel_ = modelKey;
    }

    const std::string& defaultModel() const { return defaultModel_; }

    ModelConfig getConfig(const std::string& providerKey) const
    {
        auto it = configs_.find(providerKey);
        return (it != configs_.end()) ? it->second : ModelConfig{};
    }

    bool hasConfig(const std::string& providerKey) const
    {
        return configs_.count(providerKey) > 0;
    }

private:
    std::string raw_;
    std::string defaultModel_;
    std::unordered_map<std::string, ModelConfig> configs_;

    static std::string extractString(const std::string& json, const std::string& key)
    {
        std::string k = "\"" + key + "\"";
        auto pos = json.find(k);
        if (pos == std::string::npos) return "";
        pos += k.size();
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == ':' ||
                json[pos] == '\n' || json[pos] == '\r' || json[pos] == '\t'))
            ++pos;
        if (pos >= json.size() || json[pos] != '"') return "";
        ++pos;
        std::string result;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                ++pos;
                switch (json[pos]) {
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case '"': result += '"';  break;
                    case '\\':result += '\\'; break;
                    default:  result += json[pos]; break;
                }
            }
            else result += json[pos];
            ++pos;
        }
        return result;
    }

    static int extractInt(const std::string& json, const std::string& key, int def)
    {
        std::string k = "\"" + key + "\"";
        auto pos = json.find(k);
        if (pos == std::string::npos) return def;
        pos += k.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
        if (pos >= json.size() || !std::isdigit((unsigned char)json[pos])) return def;
        int val = 0;
        while (pos < json.size() && std::isdigit((unsigned char)json[pos]))
            val = val * 10 + (json[pos++] - '0');
        return val;
    }

    static size_t findObjectEnd(const std::string& s, size_t start)
    {
        int depth = 0;
        bool inStr = false;
        for (size_t i = start; i < s.size(); ++i)
        {
            if (inStr)
            {
                if (s[i] == '\\') { ++i; continue; }
                if (s[i] == '"') inStr = false;
            }
            else
            {
                if (s[i] == '"') inStr = true;
                else if (s[i] == '{') ++depth;
                else if (s[i] == '}') { --depth; if (!depth) return i; }
            }
        }
        return s.size() - 1;
    }
};

} // namespace ai
