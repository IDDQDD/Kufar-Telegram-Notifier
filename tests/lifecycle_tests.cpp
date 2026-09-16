#include "lifecycle.hpp"
#include <iostream>

using namespace Lifecycle;
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        require(parseUserID("707549545") == 707549545, "valid ID");
        for (const auto *invalid : {"", "0", "-100123", "@name", "12 34", "123x", "4503599627370496", "99999999999999999999"})
            require(!parseUserID(invalid), "invalid ID rejected");
        Access access;
        access.owner = 1;
        access.initial = {1, 2};
        require(access.allows(1) && access.allows(2) && !access.allows(3), "initial users preserved");
        require(!access.change(2, 3, true), "non-owner cannot add users");
        require(!access.change(2, 1, false), "non-owner cannot remove owner");
        require(!access.change(1, 1, false), "owner cannot remove self");
        require(!access.change(1, -2, true), "group ID rejected");
        require(access.change(1, 3, true) && access.allows(3), "owner can add empty user");
        require(access.change(1, 2, false) && !access.allows(2), "owner can remove configured user");
        Access restarted;
        restarted.owner = 1;
        restarted.initial = {1, 2};
        restarted.load(access.save());
        require(restarted.users() == std::set<int64_t>({1, 3}), "access persists without resurrecting configured user");
        require(restarted.change(1, 2, true) && restarted.allows(2), "removed user can rejoin");
        bool rejected = false;
        try { restarted.load(nlohmann::json{{"4", "true"}}); } catch (...) { rejected = true; }
        require(rejected, "malformed access state rejected");

        const int64_t now = 2000000000;
        const auto legacy = nlohmann::json{
            {"viewed-ads", {10, 11, 12}}, {"initialized-queries", {"search"}},
            {"ad-prices", {{"10", 150}, {"11", 200}, {"12", 300}}}
        };
        auto cache = readCache(legacy, now);
        require(cache.lastSeen.at("10") == now, "legacy IDs granted full retention");
        require(cache.adLowestPrices == cache.adPrices, "legacy price fallback retained");
        cache.adLowestPrices["10"] = 100;
        auto roundtrip = readCache(writeCache(cache), now + 100);
        require(roundtrip.adLowestPrices.at("10") == 100 && roundtrip.lastSeen.at("10") == now,
                "roundtrip preserves minimum prices and dates");
        cache.lastSeen["10"] = now - 180LL * 86400 - 1;
        cache.lastSeen["11"] = now - 180LL * 86400;
        cache.lastSeen["12"] = now;
        cache.adPrices["orphan"] = 999;
        std::map<int64_t, RecipientCache> caches{{1, cache}};
        prune(caches, now, 180, 20000, 100000);
        require(caches[1].viewedAds == std::vector<int>({11, 12}), "TTL boundary and recent ads");
        require(caches[1].adPrices.size() == 2 && caches[1].adLowestPrices.size() == 2 && caches[1].lastSeen.size() == 2,
                "expired IDs and orphan metadata removed together");
        require(caches[1].initializedQueries == std::vector<std::string>({"search"}), "query priming retained");

        caches.clear();
        for (int chat = 1; chat <= 2; ++chat) {
            for (int id = 1; id <= 5; ++id) {
                caches[chat].viewedAds.push_back(id);
                caches[chat].lastSeen[std::to_string(id)] = now - 100 + chat * 10 + id;
                caches[chat].adPrices[std::to_string(id)] = 123;
                caches[chat].adLowestPrices[std::to_string(id)] = 100;
            }
        }
        prune(caches, now, 180, 3, 4);
        require(caches[1].viewedAds == std::vector<int>({5}), "global cap evicts oldest records");
        require(caches[2].viewedAds == std::vector<int>({3, 4, 5}), "per-user cap keeps latest records");
        require(caches[1].lastSeen.size() == 1 && caches[2].adLowestPrices.size() == 3, "capped metadata stays bounded");
        const auto snapshot = writeCache(caches[2]);
        prune(caches, now, 180, 3, 4);
        require(writeCache(caches[2]) == snapshot, "cleanup is idempotent");
        std::cout << "Lifecycle tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
