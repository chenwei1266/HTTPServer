#pragma once

#include "AIStrategy.h"
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <iostream>

namespace ai
{

// ════════════════════════════════════════════════════════════
//  AIFactory  —— 注册式工厂，线程安全
//
//  用法：
//    // 注册（通常在程序启动时）
//    AIFactory::instance().registerModel("doubao",
//        [](const ModelConfig& cfg) {
//            return std::make_unique<DoubaoStrategy>(cfg);
//        });
//
//    // 创建
//    auto model = AIFactory::instance().createModel("doubao", cfg);
//    model->sendStreamMsg(...);
// ════════════════════════════════════════════════════════════

class AIFactory
{
public:
    using Creator = std::function<std::unique_ptr<AIStrategy>(const ModelConfig&)>;

    // ── 单例 ──────────────────────────────────────────────
    static AIFactory& instance()
    {
        static AIFactory inst;
        return inst;
    }

    // ── 注册模型创建函数 ──────────────────────────────────
    // providerKey: 唯一键，如 "doubao" / "qwen" / "wenxin"
    // creator: 工厂函数，接收 ModelConfig 返回 unique_ptr
    void registerModel(const std::string& providerKey, Creator creator)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (registry_.count(providerKey))
        {
            std::cerr << "[AIFactory] WARNING: overwriting existing model key: "
                      << providerKey << "\n";
        }
        registry_[providerKey] = std::move(creator);
        std::cout << "[AIFactory] Registered model: " << providerKey << "\n";
    }

    // ── 创建模型实例 ──────────────────────────────────────
    // 找不到 key 时抛出 std::runtime_error
    std::unique_ptr<AIStrategy> createModel(
        const std::string& providerKey,
        const ModelConfig& config) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = registry_.find(providerKey);
        if (it == registry_.end())
        {
            throw std::runtime_error(
                "[AIFactory] Unknown model key: '" + providerKey +
                "'. Did you forget to register it?");
        }
        return it->second(config);
    }

    // 同上，但找不到时返回 nullptr（不抛异常）
    std::unique_ptr<AIStrategy> tryCreateModel(
        const std::string& providerKey,
        const ModelConfig& config) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = registry_.find(providerKey);
        if (it == registry_.end()) return nullptr;
        return it->second(config);
    }

    // ── 查询 ──────────────────────────────────────────────
    bool hasModel(const std::string& providerKey) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return registry_.count(providerKey) > 0;
    }

    std::vector<std::string> listModels() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> keys;
        keys.reserve(registry_.size());
        for (auto& [k, _] : registry_) keys.push_back(k);
        return keys;
    }

private:
    AIFactory()  = default;
    ~AIFactory() = default;
    AIFactory(const AIFactory&)            = delete;
    AIFactory& operator=(const AIFactory&) = delete;

    mutable std::mutex                          mutex_;
    std::unordered_map<std::string, Creator>    registry_;
};


} // namespace ai

// ════════════════════════════════════════════════════════════
//  自注册辅助宏（定义在 namespace ai 外部，避免 :: 拼接问题）
//
//  参数：
//    key          — 注册键字符串，如 "doubao"
//    StrategyClass — 完整类名，如 ai::DoubaoStrategy
//    UniqueTag     — 合法 C++ 标识符，用于生成唯一结构体名，
//                    如 Doubao（不能含 ::）
//
//  用法：
//    REGISTER_AI_MODEL("doubao", ai::DoubaoStrategy, Doubao)
// ════════════════════════════════════════════════════════════
#define REGISTER_AI_MODEL(key, StrategyClass, UniqueTag)                      \
    namespace {                                                                \
        struct _AutoRegister_##UniqueTag {                                     \
            _AutoRegister_##UniqueTag() {                                      \
                ::ai::AIFactory::instance().registerModel(                     \
                    key,                                                        \
                    [](const ::ai::ModelConfig& cfg) {                         \
                        return std::make_unique<StrategyClass>(cfg);           \
                    });                                                         \
            }                                                                  \
        } _autoRegister_##UniqueTag##_instance;                                \
    }