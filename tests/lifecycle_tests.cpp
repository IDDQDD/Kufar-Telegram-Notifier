#include "lifecycle.hpp"
#include "proxyhealth.hpp"
#include <iostream>

using namespace Lifecycle;
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        int64_t offset = 0;
        int64_t savedOffset = 0;
        int replies = 0;
        int writes = 0;
        const auto persist = [&]() { savedOffset = offset; ++writes; };
        for (int64_t id : {10, 10, 9, 11, 11}) {
            if (claimTelegramUpdate(id, offset, persist)) {
                require(savedOffset == id + 1, "acceptance must be durable before replying");
                ++replies;
            }
        }
        require(replies == 2 && writes == 2, "replayed and stale updates must not reply or write twice");
        offset = savedOffset; // A new process resumes from the persisted offset.
        require(!claimTelegramUpdate(11, offset, persist), "restart after reply must not replay the command");
        bool writeFailed = false;
        try {
            claimTelegramUpdate(12, offset, []() { throw std::runtime_error("disk full"); });
        } catch (const std::exception &) { writeFailed = true; }
        require(writeFailed && offset == savedOffset, "failed persistence must reject handling and roll back offset");
        require(claimTelegramUpdate(12, offset, persist), "a command can be accepted after storage is repaired");
        require(!claimTelegramUpdate(-1, offset, persist), "invalid update IDs rejected");
        require(!claimTelegramUpdate(std::numeric_limits<int64_t>::max(), offset, persist), "offset must not overflow");

        std::map<std::string, int64_t> messageIDs;
        std::map<std::string, int64_t> savedMessages;
        offset = 0;
        const auto saveDelivery = [&]() { savedOffset = offset; savedMessages = messageIDs; };
        require(claimTelegramDelivery(1, offset, 123, 40, messageIDs, saveDelivery), "first message accepted");
        require(savedOffset == 2 && savedMessages.at("123") == 40, "update and message acceptance persisted before reply");
        require(!claimTelegramDelivery(2, offset, 123, 40, messageIDs, saveDelivery), "same message under a different update cannot reply twice");
        require(offset == 3 && savedOffset == 3, "duplicate message still acknowledged to Telegram");
        messageIDs = savedMessages;
        offset = savedOffset;
        require(!claimTelegramDelivery(3, offset, 123, 40, messageIDs, saveDelivery), "message replay blocked after restart");
        require(claimTelegramDelivery(4, offset, 123, 41, messageIDs, saveDelivery), "new click on the same button accepted");
        require(claimTelegramDelivery(5, offset, 456, 40, messageIDs, saveDelivery), "message IDs belong to individual chats");
        require(!claimTelegramDelivery(6, offset, 123, 39, messageIDs, saveDelivery), "stale message blocked");
        const auto beforeFailure = messageIDs;
        const auto offsetBeforeFailure = offset;
        writeFailed = false;
        try {
            claimTelegramDelivery(7, offset, 123, 42, messageIDs, []() { throw std::runtime_error("disk full"); });
        } catch (const std::exception &) { writeFailed = true; }
        require(writeFailed && offset == offsetBeforeFailure && messageIDs == beforeFailure,
                "failed write rolls back both update and message acceptance");
        try {
            claimTelegramDelivery(7, offset, 789, 42, messageIDs, []() { throw std::runtime_error("disk full"); });
        } catch (const std::exception &) {}
        require(messageIDs == beforeFailure, "failed first message must not leave a chat record");
        require(claimTelegramDelivery(7, offset, 0, 42, messageIDs, saveDelivery) && messageIDs == beforeFailure,
                "unauthorized updates acknowledged without recording chat state");
        require(!claimTelegramDelivery(7, offset, 123, 42, messageIDs, saveDelivery) && messageIDs == beforeFailure,
                "stale update cannot change message identity");

        require(parseUserID("707549545") == 707549545, "valid ID");
        const auto namedUser = parseUserAddition(u8"707549545  Иван Петров  ");
        require(namedUser && namedUser->id == 707549545 && namedUser->name == u8"Иван Петров", "ID with a multiword name");
        require(parseUserAddition("707549545")->name.empty(), "adding by ID alone remains supported");
        require(parseUserAddition(u8"707549545\tАлексей 🚀")->name == u8"Алексей 🚀", "Unicode names accepted");
        for (const auto *invalid : {u8"Иван 707549545", "0 name", "123x name", "123 Ivan\n456 Peter", "123 Ivan\tPeter"})
            require(!parseUserAddition(invalid), "malformed ID or multiline name rejected");
        std::string longName;
        for (int i = 0; i < 80; ++i) longName += u8"Я";
        require(parseUserAddition("123 " + longName).has_value(), "name length counts Unicode characters");
        require(!parseUserAddition("123 " + longName + u8"Я"), "overlong names rejected");
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
        require(access.setName(1, 3, u8"Иван Петров"), "owner can name an enabled user");
        require(!access.setName(2, 3, "other") && !access.setName(1, 4, "unknown"), "names do not grant access or bypass ownership");
        require(access.change(1, 2, false) && !access.allows(2), "owner can remove configured user");
        Access restarted;
        restarted.owner = 1;
        restarted.initial = {1, 2};
        restarted.load(access.save());
        restarted.loadNames(access.saveNames());
        require(restarted.names.at(3) == u8"Иван Петров", "names persist through JSON and restart");
        require(restarted.users() == std::set<int64_t>({1, 3}), "access persists without resurrecting configured user");
        require(restarted.change(1, 2, true) && restarted.allows(2), "removed user can rejoin");
        bool rejected = false;
        try { restarted.load(nlohmann::json{{"4", "true"}}); } catch (...) { rejected = true; }
        require(rejected, "malformed access state rejected");

        // Churning menu users must not accumulate denial records or empty query lists.
        auto queryOverrides = nlohmann::json::object();
        for (int64_t id = 100; id < 1100; ++id) {
            require(restarted.change(1, id, true), "add churn user");
            require(restarted.setName(1, id, "temporary"), "name churn user");
            require(restarted.change(1, id, false), "remove churn user");
            queryOverrides[std::to_string(id)] = nlohmann::json::array();
        }
        restarted.compact();
        compactQueryOverrides(queryOverrides, restarted);
        require(restarted.overrides.size() == 1 && queryOverrides.empty(), "removed menu users leave no growing state");
        require(restarted.names.size() == 1 && restarted.names.at(3) == u8"Иван Петров", "removal forgets names without affecting another user");
        restarted.change(1, 2, false);
        queryOverrides["2"] = nlohmann::json::array();
        restarted.compact();
        compactQueryOverrides(queryOverrides, restarted);
        Access afterRestart;
        afterRestart.owner = 1;
        afterRestart.initial = {1, 2};
        afterRestart.load(restarted.save());
        require(!afterRestart.allows(2) && queryOverrides.contains("2"), "configured user remains disabled after compaction and restart");
        require(afterRestart.change(1, 2, true) && queryOverrides["2"].empty(), "reenabling configured user retains empty search override");
        restarted.load(nlohmann::json{{"9999", false}});
        restarted.compact();
        require(!restarted.save().contains("9999"), "legacy menu-user denials compacted");

        ProxyHealth health;
        for (int i = 0; i < 4; ++i) health.failed();
        require(!health.pending() && health.failures == 0, "direct requests never raise proxy alerts");
        health.key = "test-proxy";
        health.failed(); health.failed();
        require(!health.pending(), "two failures do not alert");
        ProxyHealth resumed;
        resumed.key = health.key;
        resumed.load(health.save());
        resumed.failed();
        require(resumed.pending(), "third consecutive failure alerts after a restart too");
        resumed.notified = true;
        health.load(resumed.save());
        health.failed();
        require(!health.pending(), "an outage only alerts once across restarts");
        health.reset(); health.failed(); health.failed(); health.failed();
        require(health.pending(), "recovery allows a future outage alert");
        resumed.key = "replacement-proxy";
        resumed.load(health.save());
        require(resumed.failures == 0 && !resumed.pending(), "changing the proxy starts a fresh health record");

        const int64_t now = 2000000000;
        const auto legacy = nlohmann::json{
            {"viewed-ads", {10, 11, 12}}, {"initialized-queries", {"search"}},
            {"ad-prices", {{"10", 150}, {"11", 200}, {"12", 300}}}
        };
        auto cache = readCache(legacy, now);
        require(cache.lastSeen.at("10") == now, "legacy IDs granted full retention");
        require(cache.adLowestPrices == cache.adPrices, "legacy price fallback retained");
        require(handledByQuery(cache, 10, "any-search"), "legacy history must keep suppressing duplicates");
        cache.initialOnlyAds["10"] = {"new-search"};
        require(handledByQuery(cache, 10, "new-search") && !handledByQuery(cache, 10, "existing-search"),
                "silently priming one query must not claim delivery for another query");
        auto silentRoundtrip = readCache(writeCache(cache), now + 1);
        require(silentRoundtrip.initialOnlyAds == cache.initialOnlyAds,
                "silent baseline ownership must survive a restart");
        silentRoundtrip.initialOnlyAds.erase("10");
        require(handledByQuery(silentRoundtrip, 10, "existing-search"), "confirmed delivery suppresses duplicates everywhere");
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
        require(caches[1].initialOnlyAds.empty(), "expired silent baselines must be pruned with their IDs");
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
