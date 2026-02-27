#pragma once

#include <string>
#include <vector>
#include <memory>
#include <algorithm>

#include "../include/utils/db/DbConnectionPool.h"

namespace dao
{

struct ChatMessage
{
    int64_t     id = 0;
    int64_t     conversationId = 0;
    std::string role;       // "user" | "assistant" | "system"
    std::string content;
    std::string createdAt;
};

class MessageDao
{
public:
    // 插入一条消息，返回消息 id
    static int64_t insert(int64_t conversationId, const std::string& role,
                          const std::string& content)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->executeUpdate(
            "INSERT INTO messages (conversation_id, role, content) VALUES (?, ?, ?)",
            conversationId, role, content);

        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery("SELECT LAST_INSERT_ID() AS id"));
        if (rs && rs->next())
            return rs->getInt64("id");
        return 0;
    }

    // 获取某会话的所有消息，按时间正序
    static std::vector<ChatMessage> listByConversation(int64_t conversationId)
    {
        std::vector<ChatMessage> result;
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery(
                "SELECT id, conversation_id, role, content, created_at "
                "FROM messages WHERE conversation_id = ? ORDER BY created_at ASC",
                conversationId));

        while (rs && rs->next())
        {
            ChatMessage m;
            m.id             = rs->getInt64("id");
            m.conversationId = rs->getInt64("conversation_id");
            m.role           = rs->getString("role");
            m.content        = rs->getString("content");
            m.createdAt      = rs->getString("created_at");
            result.push_back(std::move(m));
        }
        return result;
    }

    // 获取某会话最近 N 条消息（用于构造 LLM 上下文）
    static std::vector<ChatMessage> listRecent(int64_t conversationId, int limit = 50)
    {
        std::vector<ChatMessage> result;
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery(
                "SELECT id, conversation_id, role, content, created_at "
                "FROM messages WHERE conversation_id = ? "
                "ORDER BY created_at DESC LIMIT ?",
                conversationId, limit));

        while (rs && rs->next())
        {
            ChatMessage m;
            m.id             = rs->getInt64("id");
            m.conversationId = rs->getInt64("conversation_id");
            m.role           = rs->getString("role");
            m.content        = rs->getString("content");
            m.createdAt      = rs->getString("created_at");
            result.push_back(std::move(m));
        }
        // 反转为正序
        std::reverse(result.begin(), result.end());
        return result;
    }

    // 获取会话消息数量
    static int64_t count(int64_t conversationId)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery(
                "SELECT COUNT(*) AS cnt FROM messages WHERE conversation_id = ?",
                conversationId));
        if (rs && rs->next())
            return rs->getInt64("cnt");
        return 0;
    }
};

} // namespace dao
