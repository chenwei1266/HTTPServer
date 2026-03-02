#pragma once

// ════════════════════════════════════════════════════════════
//  ModelRegister.h
//  在 main() 或服务启动前 #include 这一个文件
//  所有厂商 Strategy 自动完成注册，无需手动调用
// ════════════════════════════════════════════════════════════

#include "AIFactory.h"
#include "AIStrategies.h"

// 利用静态对象初始化时序完成自动注册
// 第三个参数是合法 C++ 标识符（不能含 ::），用于生成唯一结构体名
REGISTER_AI_MODEL("claude", ai::ClaudeStrategy, Claude)
REGISTER_AI_MODEL("doubao", ai::DoubaoStrategy, Doubao)
REGISTER_AI_MODEL("qwen",   ai::QwenStrategy,   Qwen)
REGISTER_AI_MODEL("wenxin", ai::WenxinStrategy, Wenxin)