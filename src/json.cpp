#include "json.hpp"

#include <cstdio>

std::string JsonObject::escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

void JsonObject::key_(const std::string& key)
{
    if (!body_.empty()) body_ += ',';
    body_ += escape(key);
    body_ += ':';
}

JsonObject& JsonObject::add(const std::string& key, const std::string& value)
{
    key_(key);
    body_ += escape(value);
    return *this;
}

JsonObject& JsonObject::add(const std::string& key, const char* value)
{
    return add(key, std::string(value ? value : ""));
}

JsonObject& JsonObject::add(const std::string& key, int64_t value)
{
    key_(key);
    body_ += std::to_string(value);
    return *this;
}

JsonObject& JsonObject::add(const std::string& key, bool value)
{
    key_(key);
    body_ += value ? "true" : "false";
    return *this;
}
