#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <type_traits>
#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <mysql_driver.h>
#include <mysql/mysql.h>
#include <muduo/base/Logging.h>
#include "DbException.h"

namespace http 
{
namespace db 
{

class DbConnection 
{
public:
    DbConnection(const std::string& host, 
                const std::string& user,
                const std::string& password,
                const std::string& database);
    ~DbConnection();

    // 禁止拷贝
    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    bool isValid();
    void reconnect();
    void cleanup();

    template<typename... Args>
    sql::ResultSet* executeQuery(const std::string& sql, Args&&... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try 
        {
            std::unique_ptr<sql::PreparedStatement> stmt(
                conn_->prepareStatement(sql)
            );
            bindParams(stmt.get(), 1, std::forward<Args>(args)...);
            return stmt->executeQuery();
        } 
        catch (const sql::SQLException& e) 
        {
            LOG_ERROR << "Query failed: " << e.what() << ", SQL: " << sql;
            throw DbException(e.what());
        }
    }
    
    template<typename... Args>
    int executeUpdate(const std::string& sql, Args&&... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try 
        {
            std::unique_ptr<sql::PreparedStatement> stmt(
                conn_->prepareStatement(sql)
            );
            bindParams(stmt.get(), 1, std::forward<Args>(args)...);
            return stmt->executeUpdate();
        } 
        catch (const sql::SQLException& e) 
        {
            LOG_ERROR << "Update failed: " << e.what() << ", SQL: " << sql;
            throw DbException(e.what());
        }
    }

    bool ping();

private:
    // 递归终止
    void bindParams(sql::PreparedStatement*, int) {}

    // string 字面量 / const char* 重载（优先级最高，避免走数值模板）
    template<typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index,
                    const char* value, Args&&... args)
    {
        stmt->setString(index, value);
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }

    // const std::string& 重载
    template<typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index,
                    const std::string& value, Args&&... args)
    {
        stmt->setString(index, value);
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }

    // std::string& 重载（非 const，防止落入数值模板）
    template<typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index,
                    std::string& value, Args&&... args)
    {
        stmt->setString(index, value);
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }

    // std::string&& 重载（右值）
    template<typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index,
                    std::string&& value, Args&&... args)
    {
        stmt->setString(index, value);
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }

    // 数值类型通用模板（编译期拦截非数值、非字符串类型）
    template<typename T, typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index,
                    T&& value, Args&&... args)
    {
        static_assert(std::is_arithmetic<std::remove_reference_t<T>>::value,
                      "bindParams: unsupported parameter type, "
                      "only std::string, const char*, and arithmetic types are allowed");
        stmt->setString(index, std::to_string(std::forward<T>(value)));
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }

private:
    std::shared_ptr<sql::Connection> conn_;
    std::string                      host_;
    std::string                      user_;
    std::string                      password_;
    std::string                      database_;
    std::mutex                       mutex_;
};

} // namespace db
} // namespace http