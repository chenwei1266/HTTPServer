#pragma once

// ════════════════════════════════════════════════════════════
//  MCPAgent.h  —— MCP 两段式调度核心
//
//  流程：
//  ┌─────────────────────────────────────────────────────┐
//  │  用户消息进入                                        │
//  │     ↓                                               │
//  │  [第一段] 在 System Prompt 注入工具描述               │
//  │     ↓                                               │
//  │  调用模型（流式）                                    │
//  │     ↓                                               │
//  │  模型返回中是否含 TOOL_CALL?                         │
//  │     ├── 否 → 直接流式输出给前端（普通回答）           │
//  │     └── 是 ↓                                        │
//  │  [第二段] 解析工具调用意图                            │
//  │     ↓                                               │
//  │  执行本地工具函数                                    │
//  │     ↓                                               │
//  │  将工具结果拼回 messages，追加为 user 消息            │
//  │     ↓                                               │
//  │  再次调用模型（流式），生成最终回答                   │
//  │     ↓                                               │
//  │  流式输出最终回答给前端                              │
//  └─────────────────────────────────────────────────────┘
// ════════════════════════════════════════════════════════════

#include "MCPToolRegistry.h"
#include "../ai/AIStrategy.h"
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <iostream>

namespace mcp
{

class MCPAgent
{
public:
    using TokenCallback = ai::TokenCallback;
    using DoneCallback  = ai::DoneCallback;
    using ErrorCallback = ai::ErrorCallback;

    // maxRounds：最多执行几轮工具调用（防止死循环）
    explicit MCPAgent(int maxRounds = 3)
        : maxRounds_(maxRounds)
    {}

    // ════════════════════════════════════════════════════
    //  主入口：带工具能力的流式对话
    //
    //  strategy    — 已创建好的模型策略
    //  messages    — 原始对话上下文（不含工具描述）
    //  onToken     — 流式 token 回调（只在最终回答时触发）
    //  onDone      — 完成回调
    //  onError     — 错误回调
    //  onToolCall  — 工具调用通知回调（用于推送给前端展示）
    // ════════════════════════════════════════════════════
    void chat(
        ai::AIStrategy*              strategy,
        const std::vector<ai::Message>& messages,
        TokenCallback    onToken,
        DoneCallback     onDone,
        ErrorCallback    onError,
        std::function<void(const std::string& toolName,
                           const std::string& result)> onToolCall = nullptr)
    {
        auto& registry = MCPToolRegistry::instance();

        // ─── 第一段：注入工具描述到 System Prompt ────────
        std::vector<ai::Message> augmented = injectToolsIntoMessages(messages);

        // ─── 开始多轮调度 ─────────────────────────────────
        runRound(strategy, augmented, 0,
                 onToken, onDone, onError, onToolCall);
    }

private:
    int maxRounds_;

    // ── 将工具描述注入到 System Prompt ───────────────────
    std::vector<ai::Message> injectToolsIntoMessages(
        const std::vector<ai::Message>& messages) const
    {
        auto& registry = MCPToolRegistry::instance();
        std::string toolPrompt = registry.buildSystemPrompt();

        if (toolPrompt.empty())
            return messages; // 没有注册任何工具，不修改

        std::vector<ai::Message> result;

        // 查找是否已有 system 消息
        bool hasSystem = false;
        for (auto& m : messages)
        {
            if (m.role == "system")
            {
                // 追加到现有 system 消息末尾
                result.push_back({
                    "system",
                    m.content + "\n\n" + toolPrompt
                });
                hasSystem = true;
            }
            else
            {
                result.push_back(m);
            }
        }

        if (!hasSystem)
        {
            // 在最前面插入 system 消息
            result.insert(result.begin(), {"system", toolPrompt});
        }

        return result;
    }

    // ── 单轮调度：调用模型 → 收集完整回复 → 判断是否工具调用 ──
    void runRound(
        ai::AIStrategy*              strategy,
        std::vector<ai::Message>     messages,
        int                          round,
        TokenCallback                onToken,
        DoneCallback                 onDone,
        ErrorCallback                onError,
        std::function<void(const std::string&,
                           const std::string&)> onToolCall)
    {
        if (round >= maxRounds_)
        {
            // 超出轮次上限，直接告知
            onToken("（工具调用轮次已达上限，直接回答）\n");
            finalStream(strategy, messages, onToken, onDone, onError);
            return;
        }

        // 先用同步接口收集完整的模型输出，再判断是否含工具调用
        // 注意：这里用 sendMessage（同步），不走流式
        // 只有最终回答才走流式推送给前端
        auto result = strategy->sendMessage(messages);

        if (!result.success)
        {
            onError(result.error);
            return;
        }

        // ─── 检测是否含 TOOL_CALL ────────────────────────
        auto& registry = MCPToolRegistry::instance();
        ToolCall toolCall = registry.parseToolCall(result.content);

        if (!toolCall.valid)
        {
            // 没有工具调用 → 直接流式输出这个回复
            // 因为已经有完整内容了，模拟流式逐 token 推送
            streamText(result.content, onToken, onDone);
            return;
        }

        // ─── 有工具调用 ──────────────────────────────────
        std::cout << "[MCP] Round " << round + 1
                  << " - Tool call detected: " << toolCall.toolName << "\n";

        // 通知前端正在调用工具（会显示在 UI 上）
        if (onToolCall)
            onToolCall(toolCall.toolName, "calling");

        // 执行工具
        ToolResult toolResult = registry.executeTool(toolCall);

        std::cout << "[MCP] Tool result: "
                  << (toolResult.success ? toolResult.content : toolResult.error)
                  << "\n";

        // 通知前端工具执行结果
        if (onToolCall)
        {
            onToolCall(toolCall.toolName,
                       toolResult.success ? toolResult.content : "failed: " + toolResult.error);
        }

        // ─── 将工具调用意图和结果拼回上下文 ─────────────
        // 1. 把模型的 TOOL_CALL 输出作为 assistant 消息
        messages.push_back({"assistant", result.content});

        // 2. 把工具结果作为 user 消息（告诉模型工具已执行）
        std::string toolMsg = MCPToolRegistry::buildToolResultMessage(
            toolCall, toolResult);
        messages.push_back({"user", toolMsg});

        // ─── 递归进行下一轮 ──────────────────────────────
        runRound(strategy, messages, round + 1,
                 onToken, onDone, onError, onToolCall);
    }

    // ── 最终流式输出（已有完整文本，逐字符模拟推送）──────
    static void streamText(
        const std::string& text,
        TokenCallback onToken,
        DoneCallback  onDone)
    {
        // 注意：text 是 UTF-8，不能按字节切分，否则会出现乱码（�）
        // 这里按“代码点”切分，保证每次推送都是合法 UTF-8 片段。
        const size_t batchCodePoints = 8;
        size_t i = 0;
        while (i < text.size())
        {
            size_t start = i;
            size_t cp = 0;
            while (i < text.size() && cp < batchCodePoints)
            {
                unsigned char c = static_cast<unsigned char>(text[i]);
                size_t step = 1;
                if ((c & 0x80) == 0x00) step = 1;          // 0xxxxxxx
                else if ((c & 0xE0) == 0xC0) step = 2;     // 110xxxxx
                else if ((c & 0xF0) == 0xE0) step = 3;     // 1110xxxx
                else if ((c & 0xF8) == 0xF0) step = 4;     // 11110xxx

                // 避免越界：异常 UTF-8 时退化为单字节推进
                if (i + step > text.size()) step = 1;

                i += step;
                ++cp;
            }

            onToken(text.substr(start, i - start));

            // 模拟流式节奏，避免内核合并后前端一次性显示
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        onDone();
    }

    // ── 最后一轮用流式接口调用模型（有工具结果时） ────────
    static void finalStream(
        ai::AIStrategy*              strategy,
        const std::vector<ai::Message>& messages,
        TokenCallback onToken,
        DoneCallback  onDone,
        ErrorCallback onError)
    {
        strategy->sendStreamMsg(messages, onToken, onDone, onError);
    }
};

} // namespace mcp
