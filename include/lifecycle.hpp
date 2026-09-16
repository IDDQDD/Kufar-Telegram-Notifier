#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
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

struct Access {
    int64_t owner = 0;
    std::set<int64_t> initial;
    std::map<int64_t, bool> overrides;

    bool allows(int64_t id) const {
        if (id <= 0) return false;
        if (id == owner) return true;
        const auto it = overrides.find(id);
        return it == overrides.end() ? initial.count(id) != 0 : it->second;
    }
    bool change(int64_t actor, int64_t target, bool enabled) {
        if (actor != owner || target == owner || !parseUserID(std::to_string(target))) return false;
        overrides[target] = enabled;
        return true;
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
};

struct RecipientCache {
    std::vector<int> viewedAds;
    std::vector<std::string> initializedQueries;
    std::map<std::string, int> adPrices;
    std::map<std::string, int> adLowestPrices;
    std::map<std::string, int64_t> lastSeen;
};

inline RecipientCache readCache(const nlohmann::json &data, int64_t now) {
    RecipientCache result;
    result.viewedAds = data.value("viewed-ads", std::vector<int>{});
    result.initializedQueries = data.value("initialized-queries", std::vector<std::string>{});
    result.adPrices = data.value("ad-prices", std::map<std::string, int>{});
    result.adLowestPrices = data.value("ad-lowest-prices", result.adPrices);
    result.lastSeen = data.value("last-seen", std::map<std::string, int64_t>{});
    // Old caches have no dates: grant them a full retention period.
    for (int id : result.viewedAds) result.lastSeen.emplace(std::to_string(id), now);
    return result;
}

inline nlohmann::json writeCache(const RecipientCache &cache) {
    return {{"viewed-ads", cache.viewedAds}, {"initialized-queries", cache.initializedQueries},
            {"ad-prices", cache.adPrices}, {"ad-lowest-prices", cache.adLowestPrices},
            {"last-seen", cache.lastSeen}};
}

inline void retain(RecipientCache &cache, const std::set<int> &keep) {
    cache.viewedAds.assign(keep.begin(), keep.end());
    std::set<std::string> keys;
    for (int id : keep) keys.insert(std::to_string(id));
    const auto clean = [&](auto &entries) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (!keys.count(it->first)) it = entries.erase(it); else ++it;
        }
    };
    clean(cache.adPrices);
    clean(cache.adLowestPrices);
    clean(cache.lastSeen);
}

inline void prune(std::map<int64_t, RecipientCache> &caches, int64_t now,
                  int days, size_t perUser, size_t totalLimit) {
    std::vector<std::tuple<int64_t, int64_t, int>> all;
    const int64_t cutoff = now - int64_t(days) * 86400;
    for (auto &[chat, cache] : caches) {
        std::vector<std::pair<int64_t, int>> candidates;
        std::set<int> unique(cache.viewedAds.begin(), cache.viewedAds.end());
        for (int id : unique) {
            const auto seen = cache.lastSeen.emplace(std::to_string(id), now).first->second;
            if (seen >= cutoff) candidates.emplace_back(seen, id);
        }
        std::sort(candidates.rbegin(), candidates.rend());
        if (candidates.size() > perUser) candidates.resize(perUser);
        for (const auto &[seen, id] : candidates) all.emplace_back(seen, chat, id);
    }
    std::sort(all.rbegin(), all.rend());
    if (all.size() > totalLimit) all.resize(totalLimit);
    std::map<int64_t, std::set<int>> keep;
    for (const auto &[seen, chat, id] : all) keep[chat].insert(id);
    for (auto &[chat, cache] : caches) retain(cache, keep[chat]);
}
} // namespace Lifecycle
