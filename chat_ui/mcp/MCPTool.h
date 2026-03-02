#pragma once

// ════════════════════════════════════════════════════════════
//  MCPTool.h  —— 工具定义、参数描述、调用/结果结构体
//
//  两段式协议：
//    第一段：在 System Prompt 中注入工具描述，模型决定调用哪个工具
//    第二段：解析模型返回的 TOOL_CALL 意图 → 执行本地函数 → 结果拼回上下文 → 再次调用模型
// ════════════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace mcp
{

// ─── 工具参数定义 ─────────────────────────────────────────────
struct ToolParam
{
    std::string name;           // 参数名，如 "city"
    std::string type;           // "string" | "number" | "boolean"
    std::string description;    // 参数描述，注入给模型
    bool        required = true;
};

// ─── 工具定义 ─────────────────────────────────────────────────
struct ToolDefinition
{
    std::string              name;           // 唯一工具名，如 "get_weather"
    std::string              description;    // 功能描述，注入给模型
    std::vector<ToolParam>   params;         // 参数列表
};

// ─── 工具调用请求（模型返回后解析出来）──────────────────────
struct ToolCall
{
    bool        valid = false;
    std::string toolName;                               // 模型想调用的工具名
    std::unordered_map<std::string, std::string> args;  // 参数 key→value
};

// ─── 工具执行结果 ─────────────────────────────────────────────
struct ToolResult
{
    bool        success = false;
    std::string content;    // 返回给模型的内容（自然语言描述）
    std::string error;
};

// ─── 工具执行函数类型 ─────────────────────────────────────────
// 接收参数 map，返回 ToolResult
using ToolExecutor = std::function<ToolResult(
    const std::unordered_map<std::string, std::string>& args)>;

} // namespace mcp
