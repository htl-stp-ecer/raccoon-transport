#pragma once

#include <raccoon/scalar_f_t.hpp>
#include <raccoon/scalar_i32_t.hpp>
#include <raccoon/vector3f_t.hpp>
#include <raccoon/string_t.hpp>

#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// ============================================================================
// JSON types
// ============================================================================

using JsonValue = std::variant<int64_t, double, std::string>;
using JsonObject = std::map<std::string, JsonValue>;

// ============================================================================
// JSON value accessors
// ============================================================================

inline double getNumber(const JsonValue& v) {
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    throw std::runtime_error("Expected number in JSON");
}

inline int64_t getInt(const JsonValue& v) {
    if (auto* i = std::get_if<int64_t>(&v)) return *i;
    if (auto* d = std::get_if<double>(&v)) return static_cast<int64_t>(*d);
    throw std::runtime_error("Expected integer in JSON");
}

inline const std::string& getString(const JsonValue& v) {
    return std::get<std::string>(v);
}

// ============================================================================
// Minimal JSON parser for flat objects
// ============================================================================

inline JsonObject parseJson(const std::string& json) {
    JsonObject result;

    size_t pos = json.find('{');
    size_t end = json.rfind('}');
    if (pos == std::string::npos || end == std::string::npos) return result;
    pos++;

    while (pos < end) {
        while (pos < end && std::isspace(json[pos])) pos++;
        if (pos >= end) break;

        // Key (quoted string)
        if (json[pos] != '"') break;
        pos++;
        size_t keyEnd = json.find('"', pos);
        std::string key = json.substr(pos, keyEnd - pos);
        pos = keyEnd + 1;

        // Skip ':' and whitespace
        while (pos < end && (std::isspace(json[pos]) || json[pos] == ':')) pos++;

        // Value
        if (json[pos] == '"') {
            pos++;
            std::string val;
            while (pos < end && json[pos] != '"') {
                if (json[pos] == '\\' && pos + 1 < end) {
                    pos++;
                    switch (json[pos]) {
                        case '"':  val += '"';  break;
                        case '\\': val += '\\'; break;
                        case 'n':  val += '\n'; break;
                        case 't':  val += '\t'; break;
                        default:   val += json[pos]; break;
                    }
                } else {
                    val += json[pos];
                }
                pos++;
            }
            pos++; // closing '"'
            result[key] = val;
        } else {
            size_t valStart = pos;
            while (pos < end && json[pos] != ',' && !std::isspace(json[pos])) pos++;
            std::string numStr = json.substr(valStart, pos - valStart);
            if (numStr.find('.') != std::string::npos ||
                numStr.find('e') != std::string::npos ||
                numStr.find('E') != std::string::npos) {
                result[key] = std::stod(numStr);
            } else {
                result[key] = std::stoll(numStr);
            }
        }

        while (pos < end && (std::isspace(json[pos]) || json[pos] == ',')) pos++;
    }

    return result;
}

// ============================================================================
// JSON output helpers
// ============================================================================

inline std::string formatFloat(float f) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    return buf;
}

inline std::string escapeJsonString(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

inline void printEvent(const std::string& json) {
    std::cout << json << '\n' << std::flush;
}

// ============================================================================
// Hex encode/decode
// ============================================================================

inline std::string bytesToHex(const uint8_t* data, int len) {
    std::ostringstream oss;
    for (int i = 0; i < len; i++) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

inline std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(
            static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

// ============================================================================
// Message encoding
// ============================================================================

template <typename T>
inline std::vector<uint8_t> encodeMsg(const T& msg) {
    int maxLen = msg.getEncodedSize();
    std::vector<uint8_t> buf(maxLen);
    msg.encode(buf.data(), 0, maxLen);
    return buf;
}

inline std::vector<uint8_t> createAndEncode(const std::string& type,
                                            const JsonObject& values) {
    if (type == "scalar_f_t") {
        raccoon::scalar_f_t msg{};
        msg.timestamp = getInt(values.at("timestamp"));
        msg.value = static_cast<float>(getNumber(values.at("value")));
        return encodeMsg(msg);
    } else if (type == "scalar_i32_t") {
        raccoon::scalar_i32_t msg{};
        msg.timestamp = getInt(values.at("timestamp"));
        msg.value = static_cast<int32_t>(getInt(values.at("value")));
        return encodeMsg(msg);
    } else if (type == "vector3f_t") {
        raccoon::vector3f_t msg{};
        msg.timestamp = getInt(values.at("timestamp"));
        msg.x = static_cast<float>(getNumber(values.at("x")));
        msg.y = static_cast<float>(getNumber(values.at("y")));
        msg.z = static_cast<float>(getNumber(values.at("z")));
        return encodeMsg(msg);
    } else if (type == "string_t") {
        raccoon::string_t msg{};
        msg.timestamp = getInt(values.at("timestamp"));
        msg.value = getString(values.at("value"));
        return encodeMsg(msg);
    }
    throw std::runtime_error("Unknown type: " + type);
}

// ============================================================================
// Message to JSON
// ============================================================================

inline std::string messageToJson(const std::string& type, const void* data,
                                 int dataLen) {
    if (type == "scalar_f_t") {
        raccoon::scalar_f_t msg{};
        msg.decode(data, 0, dataLen);
        return "{\"timestamp\":" + std::to_string(msg.timestamp) +
               ",\"value\":" + formatFloat(msg.value) + "}";
    } else if (type == "scalar_i32_t") {
        raccoon::scalar_i32_t msg{};
        msg.decode(data, 0, dataLen);
        return "{\"timestamp\":" + std::to_string(msg.timestamp) +
               ",\"value\":" + std::to_string(msg.value) + "}";
    } else if (type == "vector3f_t") {
        raccoon::vector3f_t msg{};
        msg.decode(data, 0, dataLen);
        return "{\"timestamp\":" + std::to_string(msg.timestamp) +
               ",\"x\":" + formatFloat(msg.x) +
               ",\"y\":" + formatFloat(msg.y) +
               ",\"z\":" + formatFloat(msg.z) + "}";
    } else if (type == "string_t") {
        raccoon::string_t msg{};
        msg.decode(data, 0, dataLen);
        return "{\"timestamp\":" + std::to_string(msg.timestamp) +
               ",\"value\":\"" + escapeJsonString(msg.value) + "\"}";
    }
    throw std::runtime_error("Unknown type: " + type);
}

// ============================================================================
// Fingerprint retrieval
// ============================================================================

inline int64_t getFingerprint(const std::string& type) {
    if (type == "scalar_f_t")   return raccoon::scalar_f_t::getHash();
    if (type == "scalar_i32_t") return raccoon::scalar_i32_t::getHash();
    if (type == "vector3f_t")   return raccoon::vector3f_t::getHash();
    if (type == "string_t")     return raccoon::string_t::getHash();
    throw std::runtime_error("Unknown type: " + type);
}
