#pragma once

#include <cstdint>
#include <string>

// Minimal builder for flat JSON objects.
class JsonObject
{
public:
    JsonObject& add(const std::string& key, const std::string& value);
    JsonObject& add(const std::string& key, const char* value);
    JsonObject& add(const std::string& key, int64_t value);
    JsonObject& add(const std::string& key, uint32_t value) { return add(key, static_cast<int64_t>(value)); }
    JsonObject& add(const std::string& key, int value) { return add(key, static_cast<int64_t>(value)); }
    JsonObject& add(const std::string& key, bool value);

    // Serialised object without a trailing newline.
    std::string str() const { return "{" + body_ + "}"; }

    static std::string escape(const std::string& s);

private:
    void key_(const std::string& key);
    std::string body_;
};
