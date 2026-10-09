#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "json.hpp"

namespace Lifecycle {
// Persist acceptance before replying: repeated delivery or a restart cannot reply twice.
template<typename Persist>
inline bool claimTelegramUpdate(int64_t updateID, int64_t &nextOffset, Persist persist) {
    if (updateID < 0 || updateID < nextOffset || updateID == std::numeric_limits<int64_t>::max()) return false;
    const int64_t previousOffset = nextOffset;
    nextOffset = updateID + 1;
    try { persist(); }
    catch (...) { nextOffset = previousOffset; throw; }
    return true;
}

// A message can only trigger an action once, even if delivered under another update ID.
// New messages with the same text remain independent actions (e.g. toggling a category).
template<typename Persist>
inline bool claimTelegramDelivery(int64_t updateID, int64_t &nextOffset,
                                 int64_t chatID, int64_t messageID,
                                 std::map<std::string, int64_t> &messageIDs, Persist persist) {
    const std::string key = std::to_string(chatID);
    const auto previous = messageIDs.find(key);
    const bool existed = previous != messageIDs.end();
    const int64_t oldMessageID = existed ? previous->second : 0;
    bool fresh = true;
    const bool accepted = claimTelegramUpdate(updateID, nextOffset, [&]() {
        if (chatID > 0 && messageID > 0) {
            fresh = messageID > oldMessageID;
            if (fresh) messageIDs[key] = messageID;
        }
        try { persist(); }
        catch (...) {
            if (existed) messageIDs[key] = oldMessageID;
            else messageIDs.erase(key);
            throw;
        }
    });
    return accepted && fresh;
}

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
        // Only configured users need a persistent denial. Forget removed menu users.
        if (enabled == (initial.count(target) != 0)) overrides.erase(target);
        else overrides[target] = enabled;
        return true;
    }
    void compact() {
        for (auto it = overrides.begin(); it != overrides.end();) {
            if (it->first == owner || it->second == (initial.count(it->first) != 0))
                it = overrides.erase(it);
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
};

inline void compactQueryOverrides(nlohmann::json &queries, const Access &access) {
    for (auto it = queries.begin(); it != queries.end();) {
        const auto id = parseUserID(it.key());
        if (!id || (!access.allows(*id) && !access.initial.count(*id))) it = queries.erase(it);
        else ++it;
    }
}

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
