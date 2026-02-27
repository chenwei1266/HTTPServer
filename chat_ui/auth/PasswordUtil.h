#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

namespace auth
{

class PasswordUtil
{
public:
    // 生成随机 salt (32 字符 hex)
    static std::string generateSalt()
    {
        static const char hex[] = "0123456789abcdef";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, 15);

        std::string salt;
        salt.reserve(32);
        for (int i = 0; i < 32; ++i)
            salt += hex[dist(gen)];
        return salt;
    }

    // SHA256(salt + password) -> hex string
    static std::string hashPassword(const std::string& password, const std::string& salt)
    {
        std::string input = salt + password;
        unsigned char hash[32];

#ifdef __APPLE__
        CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), hash);
#else
        SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
#endif

        return toHex(hash, 32);
    }

    // 校验密码
    static bool verify(const std::string& password, const std::string& salt,
                       const std::string& storedHash)
    {
        return hashPassword(password, salt) == storedHash;
    }

private:
    static std::string toHex(const unsigned char* data, size_t len)
    {
        std::ostringstream oss;
        for (size_t i = 0; i < len; ++i)
            oss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(data[i]);
        return oss.str();
    }
};

} // namespace auth
