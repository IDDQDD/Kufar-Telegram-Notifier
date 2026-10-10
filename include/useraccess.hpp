#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include "json.hpp"

namespace Lifecycle {
inline std::optional<int64_t> parseUserID(const std::string &text) {
    if (text.empty() || text.size() > 16 ||
        text.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    try {
        const auto id = std::stoll(text);
        if (id > 0 && id < (int64_t{1} << 52)) return id;
    } catch (const std::exception &) {}
    return std::nullopt;
}

inline bool validUserName(const std::string &name) {
    std::size_t characters = 0;
    for (const unsigned char character : name) {
        if (character < 0x20 || character == 0x7f) return false;
        if ((character & 0xc0) != 0x80) ++characters;
    }
    return characters <= 80;
}

struct UserAddition {
    int64_t id;
    std::string name;
};

inline std::optional<UserAddition> parseUserAddition(const std::string &text) {
    const auto separator = text.find_first_of(" \t");
    const auto id = parseUserID(text.substr(0, separator));
    if (!id) return std::nullopt;
    std::string name;
    if (separator != std::string::npos) {
        const auto first = text.find_first_not_of(" \t", separator);
        if (first != std::string::npos)
            name = text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }
    if (!validUserName(name)) return std::nullopt;
    return UserAddition{*id, name};
}

struct Access {
    int64_t owner = 0;
    std::set<int64_t> initial;
    std::map<int64_t, bool> overrides;
    std::map<int64_t, std::string> names;

    bool allows(int64_t id) const {
        if (id <= 0) return false;
        if (id == owner) return true;
        const auto it = overrides.find(id);
        return it == overrides.end() ? initial.count(id) != 0 : it->second;
    }
    bool change(int64_t actor, int64_t target, bool enabled) {
        if (actor != owner || target == owner || !parseUserID(std::to_string(target))) return false;
        // Only configured users need a persistent denial. Forget removed menu users.
        if (enabled == (initial.count(target) != 0)) overrides.erase(target);
        else overrides[target] = enabled;
        if (!enabled) names.erase(target);
        return true;
    }
    bool setName(int64_t actor, int64_t target, const std::string &name) {
        if (actor != owner || !allows(target) || !validUserName(name)) return false;
        if (name.empty()) names.erase(target);
        else names[target] = name;
        return true;
    }
    void compact() {
        for (auto it = overrides.begin(); it != overrides.end();) {
            if (it->first == owner || it->second == (initial.count(it->first) != 0))
                it = overrides.erase(it);
            else ++it;
        }
        for (auto it = names.begin(); it != names.end();) {
            if (!allows(it->first)) it = names.erase(it);
            else ++it;
        }
    }
    std::set<int64_t> users() const {
        auto result = initial;
        result.insert(owner);
        for (const auto &[id, enabled] : overrides) {
            if (enabled) result.insert(id); else result.erase(id);
        }
        result.insert(owner);
        return result;
    }
    nlohmann::json save() const {
        auto result = nlohmann::json::object();
        for (const auto &[id, enabled] : overrides) result[std::to_string(id)] = enabled;
        return result;
    }
    void load(const nlohmann::json &data) {
        if (!data.is_object()) throw std::runtime_error("Invalid user access state");
        for (auto it = data.begin(); it != data.end(); ++it) {
            auto id = parseUserID(it.key());
            if (!id || !it.value().is_boolean()) throw std::runtime_error("Invalid user access entry");
            overrides[*id] = it.value().get<bool>();
        }
    }
    nlohmann::json saveNames() const {
        auto result = nlohmann::json::object();
        for (const auto &[id, name] : names) result[std::to_string(id)] = name;
        return result;
    }
    void loadNames(const nlohmann::json &data) {
        if (!data.is_object()) throw std::runtime_error("Invalid user names state");
        std::map<int64_t, std::string> restored;
        for (auto it = data.begin(); it != data.end(); ++it) {
            const auto id = parseUserID(it.key());
            if (!id || !it.value().is_string()) throw std::runtime_error("Invalid user name entry");
            const auto name = it.value().get<std::string>();
            if (!validUserName(name)) throw std::runtime_error("Invalid user name entry");
            if (allows(*id) && !name.empty()) restored[*id] = name;
        }
        names = std::move(restored);
    }
};

} // namespace Lifecycle
