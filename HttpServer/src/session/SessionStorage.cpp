#include "../include/session/SessionStorage.h"
#include <iostream>

namespace http
{

namespace session
{

void MemorySessionStorage::save(std::shared_ptr<Session> session)
{
    // 创建会话副本并存储
    sessions_[session->getId()] = session;
}

// 通过会话ID从存储中加载会话
std::shared_ptr<Session> MemorySessionStorage::load(const std::string& sessionId)
{
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end())
    {
        if (!it->second->isExpired())
        {
            return it->second;
        }
        else
        {
            // 如果会话已过期，则从存储中移除
            sessions_.erase(it);
        }
    }

    // 如果会话不存在或已过期，则返回nullptr
    return nullptr;
}

// 通过会话ID从存储中移除会话
void MemorySessionStorage::remove(const std::string& sessionId)
{
    sessions_.erase(sessionId);
}

} // namespace session
} // namespace http