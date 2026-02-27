#pragma once

#include <string>
#include <vector>
#include <memory>

#include "../include/utils/db/DbConnectionPool.h"

namespace dao
{

struct Conversation
{
    int64_t     id = 0;
    int64_t     userId = 0;
    std::string title;
    std::string createdAt;
    std::string updatedAt;
};

class ConversationDao
{
public:
    // 创建会话，返回新会话 id
    static int64_t create(int64_t userId, const std::string& title = "New Chat")
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->executeUpdate(
            "INSERT INTO conversations (user_id, title) VALUES (?, ?)",
            userId, title);

        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery("SELECT LAST_INSERT_ID() AS id"));
        if (rs && rs->next())
            return rs->getInt64("id");
        return 0;
    }

    // 获取用户的所有会话，按更新时间倒序
    static std::vector<Conversation> listByUser(int64_t userId)
    {
        std::vector<Conversation> result;
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery(
                "SELECT id, user_id, title, created_at, updated_at "
                "FROM conversations WHERE user_id = ? ORDER BY updated_at DESC",
                userId));

        while (rs && rs->next())
        {
            Conversation c;
            c.id        = rs->getInt64("id");
            c.userId    = rs->getInt64("user_id");
            c.title     = rs->getString("title");
            c.createdAt = rs->getString("created_at");
            c.updatedAt = rs->getString("updated_at");
            result.push_back(std::move(c));
        }
        return result;
    }

    // 按 id 查找（同时校验归属）
    static Conversation findById(int64_t convId, int64_t userId)
    {
        Conversation c;
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery(
                "SELECT id, user_id, title, created_at, updated_at "
                "FROM conversations WHERE id = ? AND user_id = ?",
                convId, userId));

        if (rs && rs->next())
        {
            c.id        = rs->getInt64("id");
            c.userId    = rs->getInt64("user_id");
            c.title     = rs->getString("title");
            c.createdAt = rs->getString("created_at");
            c.updatedAt = rs->getString("updated_at");
        }
        return c;
    }

    // 更新标题
    static bool updateTitle(int64_t convId, int64_t userId, const std::string& title)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        int rows = conn->executeUpdate(
            "UPDATE conversations SET title = ? WHERE id = ? AND user_id = ?",
            title, convId, userId);
        return rows > 0;
    }

    // 删除会话（级联删除消息）
    static bool remove(int64_t convId, int64_t userId)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        int rows = conn->executeUpdate(
            "DELETE FROM conversations WHERE id = ? AND user_id = ?",
            convId, userId);
        return rows > 0;
    }

    // touch updated_at
    static void touch(int64_t convId)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->executeUpdate(
            "UPDATE conversations SET updated_at = NOW() WHERE id = ?",
            convId);
    }
};

} // namespace dao
