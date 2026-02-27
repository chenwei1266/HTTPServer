#pragma once

#include <string>
#include <memory>

#include "../include/utils/db/DbConnectionPool.h"
#include "../auth/PasswordUtil.h"

namespace dao
{

struct User
{
    int64_t     id = 0;
    std::string username;
    std::string passwordHash;
    std::string salt;
    std::string createdAt;
};

class UserDao
{
public:
    // 注册，成功返回 user id，用户名已存在返回 -1
    static int64_t registerUser(const std::string& username, const std::string& password)
    {
        if (findByUsername(username).id != 0)
            return -1;

        std::string salt = auth::PasswordUtil::generateSalt();
        std::string hash = auth::PasswordUtil::hashPassword(password, salt);

        // 同一连接上 INSERT + LAST_INSERT_ID，保证拿到正确的自增 id
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->executeUpdate(
            "INSERT INTO users (username, password_hash, salt) VALUES (?, ?, ?)",
            username, hash, salt);

        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery("SELECT LAST_INSERT_ID() AS id"));
        if (rs && rs->next())
            return rs->getInt64("id");
        return 0;
    }

    // 登录验证，成功返回 User，失败返回 id=0
    static User login(const std::string& username, const std::string& password)
    {
        User user = findByUsername(username);
        if (user.id == 0)
            return user;

        if (!auth::PasswordUtil::verify(password, user.salt, user.passwordHash))
        {
            user.id = 0;
            return user;
        }
        return user;
    }

    static User findByUsername(const std::string& username)
    {
        User user;
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery(
                "SELECT id, username, password_hash, salt, created_at "
                "FROM users WHERE username = ?",
                username));

        if (rs && rs->next())
        {
            user.id           = rs->getInt64("id");
            user.username     = rs->getString("username");
            user.passwordHash = rs->getString("password_hash");
            user.salt         = rs->getString("salt");
            user.createdAt    = rs->getString("created_at");
        }
        return user;
    }

    static User findById(int64_t id)
    {
        User user;
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> rs(
            conn->executeQuery(
                "SELECT id, username, password_hash, salt, created_at "
                "FROM users WHERE id = ?",
                id));

        if (rs && rs->next())
        {
            user.id           = rs->getInt64("id");
            user.username     = rs->getString("username");
            user.passwordHash = rs->getString("password_hash");
            user.salt         = rs->getString("salt");
            user.createdAt    = rs->getString("created_at");
        }
        return user;
    }
};

} // namespace dao
