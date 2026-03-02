#pragma once

// ════════════════════════════════════════════════════════════
//  MCPToolRegistry.h  —— 工具注册表
//
//  职责：
//    1. 注册工具（定义 + 执行函数）
//    2. 生成注入 System Prompt 的工具描述文本
//    3. 解析模型返回的 TOOL_CALL 意图
//    4. 执行工具并返回结果
// ════════════════════════════════════════════════════════════

#include "MCPTool.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <iostream>

namespace mcp
{

class MCPToolRegistry
{
public:
    // ── 单例 ──────────────────────────────────────────────
    static MCPToolRegistry& instance()
    {
        static MCPToolRegistry inst;
        return inst;
    }

    // ── 注册工具 ──────────────────────────────────────────
    void registerTool(const ToolDefinition& def, ToolExecutor executor)
    {
        definitions_[def.name] = def;
        executors_[def.name]   = std::move(executor);
        std::cout << "[MCP] Registered tool: " << def.name << "\n";
    }

    bool hasTool(const std::string& name) const
    {
        return executors_.count(name) > 0;
    }

    std::vector<std::string> listTools() const
    {
        std::vector<std::string> names;
        for (auto& [k, _] : definitions_) names.push_back(k);
        return names;
    }

    // ════════════════════════════════════════════════════
    //  第一段：生成注入 System Prompt 的工具描述
    //
    //  生成格式（模型能理解的文本协议）：
    //
    //  你可以调用以下工具来获取实时信息。
    //  当需要调用工具时，请在回复中输出且仅输出以下格式（不要添加其他文字）：
    //  TOOL_CALL: {"tool":"工具名","args":{"参数名":"参数值",...}}
    //
    //  可用工具：
    //  ...
    // ════════════════════════════════════════════════════
    std::string buildSystemPrompt() const
    {
        if (definitions_.empty()) return "";

        std::string prompt =
            "你可以调用以下工具来获取实时信息。\n"
            "当你需要调用工具时，请在回复中输出且仅输出以下格式（整行，不要添加任何其他文字）：\n"
            "TOOL_CALL: {\"tool\":\"工具名\",\"args\":{\"参数名\":\"参数值\"}}\n\n"
            "调用规则：\n"
            "1. 只有在用户明确需要实时数据（如当前天气、当前时间）时才调用工具\n"
            "2. 工具调用行必须单独占一行，且是回复的全部内容\n"
            "3. 获得工具结果后，用自然语言回答用户，不要再输出 TOOL_CALL\n\n"
            "可用工具列表：\n";

        for (auto& [name, def] : definitions_)
        {
            prompt += "─────────────────────\n";
            prompt += "工具名: " + def.name + "\n";
            prompt += "功能: "   + def.description + "\n";
            if (!def.params.empty())
            {
                prompt += "参数:\n";
                for (auto& p : def.params)
                {
                    prompt += "  - " + p.name +
                              " (" + p.type + ")" +
                              (p.required ? " [必填]" : " [可选]") +
                              ": " + p.description + "\n";
                }
            }
        }
        prompt += "─────────────────────\n";
        return prompt;
    }

    // ════════════════════════════════════════════════════
    //  第二段 Step A：检测并解析模型输出的 TOOL_CALL 意图
    //
    //  输入：模型返回的完整文本
    //  返回：ToolCall（valid=false 表示没有工具调用）
    // ════════════════════════════════════════════════════
    ToolCall parseToolCall(const std::string& modelOutput) const
    {
        ToolCall result;

        // 查找 "TOOL_CALL:" 标记
        const std::string marker = "TOOL_CALL:";
        auto markerPos = modelOutput.find(marker);
        if (markerPos == std::string::npos)
            return result; // valid=false，没有工具调用

        // 提取 marker 后面的 JSON
        size_t jsonStart = markerPos + marker.size();
        while (jsonStart < modelOutput.size() &&
               (modelOutput[jsonStart] == ' ' || modelOutput[jsonStart] == '\t'))
            ++jsonStart;

        if (jsonStart >= modelOutput.size() || modelOutput[jsonStart] != '{')
            return result;

        // 找到完整 JSON 对象
        size_t jsonEnd = findObjectEnd(modelOutput, jsonStart);
        std::string json = modelOutput.substr(jsonStart, jsonEnd - jsonStart + 1);

        // 解析 "tool" 字段
        result.toolName = extractString(json, "tool");
        if (result.toolName.empty())
            return result;

        // 解析 "args" 对象
        auto argsPos = json.find("\"args\"");
        if (argsPos != std::string::npos)
        {
            argsPos = json.find('{', argsPos + 6);
            if (argsPos != std::string::npos)
            {
                size_t argsEnd = findObjectEnd(json, argsPos);
                std::string argsJson = json.substr(argsPos, argsEnd - argsPos + 1);
                result.args = parseSimpleObject(argsJson);
            }
        }

        result.valid = true;
        return result;
    }

    // ════════════════════════════════════════════════════
    //  第二段 Step B：执行工具
    // ════════════════════════════════════════════════════
    ToolResult executeTool(const ToolCall& call) const
    {
        if (!call.valid)
            return {false, "", "invalid tool call"};

        auto it = executors_.find(call.toolName);
        if (it == executors_.end())
            return {false, "", "tool not found: " + call.toolName};

        try
        {
            return it->second(call.args);
        }
        catch (const std::exception& e)
        {
            return {false, "", std::string("tool exception: ") + e.what()};
        }
    }

    // ════════════════════════════════════════════════════
    //  构造"工具结果"消息，拼回对话上下文
    //
    //  格式：
    //  [工具调用结果] 工具名: get_weather
    //  执行结果: 北京当前天气：晴，温度 18°C，湿度 45%
    // ════════════════════════════════════════════════════
    static std::string buildToolResultMessage(
        const ToolCall& call, const ToolResult& result)
    {
        std::string msg = "[工具调用结果] 工具名: " + call.toolName + "\n";
        if (result.success)
            msg += "执行结果: " + result.content;
        else
            msg += "执行失败: " + result.error;
        return msg;
    }

private:
    MCPToolRegistry()  = default;
    ~MCPToolRegistry() = default;
    MCPToolRegistry(const MCPToolRegistry&)            = delete;
    MCPToolRegistry& operator=(const MCPToolRegistry&) = delete;

    std::unordered_map<std::string, ToolDefinition> definitions_;
    std::unordered_map<std::string, ToolExecutor>   executors_;

    // ── JSON 工具方法 ──────────────────────────────────

    static size_t findObjectEnd(const std::string& s, size_t start)
    {
        int depth = 0;
        bool inStr = false;
        for (size_t i = start; i < s.size(); ++i)
        {
            if (inStr) {
                if (s[i] == '\\') { ++i; continue; }
                if (s[i] == '"')  inStr = false;
            } else {
                if (s[i] == '"')  inStr = true;
                else if (s[i] == '{') ++depth;
                else if (s[i] == '}') { --depth; if (!depth) return i; }
            }
        }
        return s.size() - 1;
    }

    static std::string extractString(const std::string& json,
                                     const std::string& key)
    {
        std::string k = "\"" + key + "\"";
        auto pos = json.find(k);
        if (pos == std::string::npos) return "";
        pos += k.size();
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == ':')) ++pos;
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

    // 解析简单 key-value 对象（值只有字符串类型）
    static std::unordered_map<std::string, std::string>
    parseSimpleObject(const std::string& json)
    {
        std::unordered_map<std::string, std::string> result;
        size_t pos = 1; // skip '{'
        while (pos < json.size())
        {
            while (pos < json.size() &&
                   (json[pos] == ' ' || json[pos] == ',' ||
                    json[pos] == '\n' || json[pos] == '\r')) ++pos;

            if (pos >= json.size() || json[pos] == '}') break;
            if (json[pos] != '"') { ++pos; continue; }

            // 读 key
            ++pos;
            std::string key;
            while (pos < json.size() && json[pos] != '"') key += json[pos++];
            ++pos; // skip closing "

            // 跳到 value
            while (pos < json.size() &&
                   (json[pos] == ' ' || json[pos] == ':')) ++pos;

            std::string value;
            if (pos < json.size() && json[pos] == '"')
            {
                ++pos;
                while (pos < json.size() && json[pos] != '"')
                {
                    if (json[pos] == '\\' && pos + 1 < json.size())
                    {
                        ++pos;
                        value += json[pos];
                    }
                    else value += json[pos];
                    ++pos;
                }
                ++pos; // skip closing "
            }
            else
            {
                // 数字或其他非字符串值
                while (pos < json.size() &&
                       json[pos] != ',' && json[pos] != '}') {
                    value += json[pos++];
                }
                // trim
                while (!value.empty() && value.back() == ' ') value.pop_back();
            }

            if (!key.empty()) result[key] = value;
        }
        return result;
    }
};

} // namespace mcp
