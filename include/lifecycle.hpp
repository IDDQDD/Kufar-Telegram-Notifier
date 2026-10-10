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
#include "useraccess.hpp"

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
    // IDs silently primed by these queries, but never confirmed as delivered.
    std::map<std::string, std::vector<std::string>> initialOnlyAds;
};

inline RecipientCache readCache(const nlohmann::json &data, int64_t now) {
    RecipientCache result;
    result.viewedAds = data.value("viewed-ads", std::vector<int>{});
    result.initializedQueries = data.value("initialized-queries", std::vector<std::string>{});
    result.adPrices = data.value("ad-prices", std::map<std::string, int>{});
    result.adLowestPrices = data.value("ad-lowest-prices", result.adPrices);
    result.lastSeen = data.value("last-seen", std::map<std::string, int64_t>{});
    result.initialOnlyAds = data.value("initial-only-ads", std::map<std::string, std::vector<std::string>>{});
    // Old caches have no dates: grant them a full retention period.
    for (int id : result.viewedAds) result.lastSeen.emplace(std::to_string(id), now);
    return result;
}

inline nlohmann::json writeCache(const RecipientCache &cache) {
    return {{"viewed-ads", cache.viewedAds}, {"initialized-queries", cache.initializedQueries},
            {"ad-prices", cache.adPrices}, {"ad-lowest-prices", cache.adLowestPrices},
            {"last-seen", cache.lastSeen}, {"initial-only-ads", cache.initialOnlyAds}};
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
    clean(cache.initialOnlyAds);
}

inline bool handledByQuery(const RecipientCache &cache, int id, const std::string &queryKey) {
    if (std::find(cache.viewedAds.begin(), cache.viewedAds.end(), id) == cache.viewedAds.end()) return false;
    const auto primed = cache.initialOnlyAds.find(std::to_string(id));
    // Legacy history has no distinction; preserve its existing duplicate suppression.
    return primed == cache.initialOnlyAds.end() ||
        std::find(primed->second.begin(), primed->second.end(), queryKey) != primed->second.end();
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
