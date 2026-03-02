#pragma once

// ════════════════════════════════════════════════════════════
//  BuiltinTools.h  —— 内置工具实现（天气、时间）
//
//  天气：调用 wttr.in 免费接口（无需 API Key）
//  时间：直接读系统时钟
// ════════════════════════════════════════════════════════════

#include "MCPToolRegistry.h"
#include <curl/curl.h>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace mcp
{

// ════════════════════════════════════════════════════════════
//  工具一：获取当前时间
// ════════════════════════════════════════════════════════════
inline ToolResult toolGetCurrentTime(
    const std::unordered_map<std::string, std::string>& args)
{
    // 支持可选的 timezone 参数（简单处理，仅展示 UTC+8）
    std::string tz = "Asia/Shanghai";
    auto it = args.find("timezone");
    if (it != args.end() && !it->second.empty())
        tz = it->second;

    // 获取当前时间（UTC+8）
    std::time_t now = std::time(nullptr);
    std::tm* local  = std::localtime(&now);

    std::ostringstream oss;
    oss << "当前时间（" << tz << "）：";
    oss << std::put_time(local, "%Y年%m月%d日 %H:%M:%S");

    // 加上星期
    const char* weekdays[] = {"周日","周一","周二","周三","周四","周五","周六"};
    oss << " " << weekdays[local->tm_wday];

    return {true, oss.str(), ""};
}

// ════════════════════════════════════════════════════════════
//  工具二：查询天气（使用 wttr.in 免费接口）
//  接口文档：https://wttr.in/:help
//  示例：https://wttr.in/Beijing?format=j1  返回 JSON
// ════════════════════════════════════════════════════════════
inline ToolResult toolGetWeather(
    const std::unordered_map<std::string, std::string>& args)
{
    auto it = args.find("city");
    if (it == args.end() || it->second.empty())
        return {false, "", "缺少必填参数: city"};

    std::string city = it->second;

    // URL 编码（简单处理空格和中文）
    std::string encodedCity;
    for (unsigned char c : city)
    {
        if (std::isalnum(c) || c == '-' || c == '_')
            encodedCity += c;
        else
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            encodedCity += buf;
        }
    }

    // 使用 wttr.in 的简洁格式：%t=温度 %C=天气描述 %h=湿度 %w=风速
    // format=j1 返回 JSON，更稳定
    std::string url = "https://wttr.in/" + encodedCity + "?format=%l:+%C+%t+%h+%w&lang=zh";

    // curl 同步请求
    CURL* curl = curl_easy_init();
    if (!curl) return {false, "", "curl init failed"};

    std::string response;
    auto writeCb = [](char* p, size_t s, size_t n, void* ud) -> size_t {
        static_cast<std::string*>(ud)->append(p, s * n);
        return s * n;
    };

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: curl/7.0");

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     static_cast<size_t(*)(char*,size_t,size_t,void*)>(writeCb));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return {false, "", std::string("网络请求失败: ") + curl_easy_strerror(res)};

    if (httpCode != 200)
        return {false, "", "天气服务返回错误: HTTP " + std::to_string(httpCode)};

    // 去掉末尾换行
    while (!response.empty() &&
           (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();

    if (response.empty())
        return {false, "", "天气服务返回空内容"};

    return {true, city + " 的天气：" + response, ""};
}

// ════════════════════════════════════════════════════════════
//  工具三：计算器（纯本地，展示非网络工具的接入方式）
// ════════════════════════════════════════════════════════════
inline ToolResult toolCalculate(
    const std::unordered_map<std::string, std::string>& args)
{
    auto it = args.find("expression");
    if (it == args.end() || it->second.empty())
        return {false, "", "缺少必填参数: expression"};

    // 极简计算器：只支持两个数的四则运算
    // 生产环境可接入 exprtk 等库
    const std::string& expr = it->second;
    double a = 0, b = 0;
    char op = 0;

    std::istringstream iss(expr);
    if (!(iss >> a >> op >> b))
        return {false, "", "表达式格式错误，支持: 数字 运算符 数字，如 3.14 * 2"};

    double result = 0;
    switch (op)
    {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0) return {false, "", "除数不能为零"};
            result = a / b;
            break;
        default:
            return {false, "", std::string("不支持的运算符: ") + op};
    }

    std::ostringstream oss;
    oss << expr << " = " << result;
    return {true, "计算结果：" + oss.str(), ""};
}

// ════════════════════════════════════════════════════════════
//  registerBuiltinTools()  —— 一键注册所有内置工具
//  在 main() 启动时调用一次
// ════════════════════════════════════════════════════════════
inline void registerBuiltinTools()
{
    auto& reg = MCPToolRegistry::instance();

    // ── 工具一：当前时间 ──────────────────────────────────
    reg.registerTool(
        {
            "get_current_time",
            "获取当前日期和时间",
            {
                {"timezone", "string", "时区，默认 Asia/Shanghai", false}
            }
        },
        toolGetCurrentTime
    );

    // ── 工具二：查询天气 ──────────────────────────────────
    reg.registerTool(
        {
            "get_weather",
            "查询指定城市的实时天气（温度、天气状况、湿度、风速）",
            {
                {"city", "string", "城市名称，支持中文或英文，如 北京 或 Beijing", true}
            }
        },
        toolGetWeather
    );

    // ── 工具三：计算器 ────────────────────────────────────
    reg.registerTool(
        {
            "calculate",
            "执行基本数学计算（加减乘除）",
            {
                {"expression", "string", "数学表达式，格式：数字 运算符 数字，如 \"3.14 * 2\"", true}
            }
        },
        toolCalculate
    );
}

} // namespace mcp
