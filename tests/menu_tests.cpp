#define main kufarNotifierApplicationMain
#include "../src/main.cpp"
#undef main

// Exercise the real menu loop with an offline Telegram transport.
namespace {
json incoming;
vector<json> replies;
string statePath;
int64_t expectedOffset = 0;
bool scheduleTest = false;
vector<json> batches;
size_t nextBatch = 0;
int searchCount = 0;
int searchFailuresRemaining = 0;
int sendFailuresRemaining = 0;
bool probeTest = false;
bool probeFailure = false;
bool overlappingSearches = false;
bool proxyTest = false;
vector<int> proxyOutcomes; // 0: success, 1: proxy transport failure, 2: Kufar HTTP 403.
vector<size_t> proxyAlertAttempts;
int proxyAlertSendFailures = 0;
void require(bool condition, const char *message) {
    if (!condition) throw runtime_error(message);
}
json message(int64_t updateID, int64_t messageID, const string &text) {
    return {{"update_id", updateID}, {"message", {{"message_id", messageID},
        {"chat", {{"id", 123}, {"type", "private"}}}, {"from", {{"id", 123}}}, {"text", text}}}};
}
}

namespace Networking {
string urlEncode(const string &value) { return value; }
string getJSONFromURL(const string &url) {
    if (probeTest) {
        require(url.find("size=1") != string::npos && url.find(u8"query=книга") != string::npos,
                "standalone probe must only check a small filtered search");
        ++searchCount;
        if (probeFailure) throw HTTPError(403, u8"локации, где доступ ограничен");
        return R"({"ads":[]})";
    }
    if (scheduleTest && url.find("/getUpdates?") == string::npos) {
        require(url.find(u8"query=Гиря") != string::npos && url.find("size=30") != string::npos &&
                url.find("sort=lst.d") != string::npos,
                "monitoring must request a bounded result page for the saved search");
        if (proxyTest) {
            const int outcome = proxyOutcomes.at(nextBatch - 1);
            if (outcome == 1) throw ProxyError("Offline proxy connection failed");
            if (outcome == 2) throw HTTPError(403, "Forbidden");
            return R"({"ads":[]})";
        }
        if (searchFailuresRemaining > 0) {
            --searchFailuresRemaining;
            throw HTTPError(403, "Forbidden");
        }
        ++searchCount;
        const auto advert = [](int id, const string &title) {
            return json{{"ad_id", id}, {"subject", title}, {"list_time", "2026-10-09T12:00:00Z"},
                        {"price_byn", "10000"}, {"ad_link", "https://example.test/" + to_string(id)}};
        };
        auto ads = json::array({advert(501, u8"Гиря старая")});
        if (searchCount > 1 || overlappingSearches) ads.push_back(advert(502, u8"Гиря новая"));
        return json{{"ads", ads}}.dump();
    }
    require(url.find("/getUpdates?timeout=0&offset=" + to_string(expectedOffset)) != string::npos,
            "menu must resume from the persisted offset");
    if (scheduleTest) {
        require(nextBatch < batches.size(), "unexpected extra polling");
        const auto batch = batches[nextBatch++];
        for (const auto &update : batch) expectedOffset = update.at("update_id").get<int64_t>() + 1;
        if (nextBatch == batches.size()) stopping = 1;
        return json{{"ok", true}, {"result", batch}}.dump();
    }
    stopping = 1; // Process this batch and exit; no sleeping or live API calls.
    return json{{"ok", true}, {"result", incoming}}.dump();
}
string getJSONFromURL(const string &url, const vector<string> &) {
    require(scheduleTest || probeTest, "Unexpected Kufar request in offline menu test");
    return getJSONFromURL(url);
}
string postJSONToURL(const string &url, const string &body) {
    require(!probeTest, "standalone probe must never contact Telegram");
    if (url.find("/sendMessage") != string::npos) {
        const auto request = json::parse(body);
        if (proxyTest && request.at("text").get<string>().find(u8"⚠️ Не удаётся подключиться к Kufar через настроенный прокси") == 0) {
            require(request.at("chat_id") == 123, "proxy outage alerts go only to the owner");
            proxyAlertAttempts.push_back(nextBatch - 1);
            if (proxyAlertSendFailures > 0) {
                --proxyAlertSendFailures;
                return R"({"ok":false,"description":"Offline alert send failure"})";
            }
        }
        const auto persisted = getJSONDataFromPath(statePath);
        if (!request.contains("parse_mode")) {
            require(persisted.at("telegram-update-offset").get<int64_t>() > 0 &&
                    persisted.at("telegram-message-ids").at("123").get<int64_t>() > 0,
                    "both Telegram identities must be saved before replying to a menu action");
        }
        if (request.contains("parse_mode") && sendFailuresRemaining > 0) {
            --sendFailuresRemaining;
            return R"({"ok":false,"description":"Offline send failure"})";
        }
        replies.push_back(request);
    } else {
        require(url.find("/setMyCommands") != string::npos, "unexpected Telegram operation");
    }
    return R"({"ok":true,"result":true})";
}
string postDocumentToURL(const string &, int64_t, const string &, const string &, const string &) {
    throw runtime_error("Unexpected document upload in offline menu test");
}
}

void testUserManagement(const filesystem::path &directory) {
    const auto previousPath = statePath;
    const auto previousOffset = expectedOffset;
    statePath = (directory / "users-cache.json").string();
    const auto configPath = (directory / "users-config.json").string();
    saveFile(statePath, json{{"user-access", {{"777", true}}}}.dump());
    saveFile(configPath, json{{"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
        {"queries", json::array()}, {"recipients", {
            {{"chat-id", 123}, {"queries", json::array()}},
            {{"chat-id", 456}, {"queries", {{{"tag", u8"Гиря"}}}}}
        }}}.dump());
    vector<string> arguments = {"offline-test", "--config=" + configPath, "--cache=" + statePath};
    vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    const auto run = [&](int64_t offset, const json &updates) {
        incoming = updates;
        expectedOffset = offset;
        replies.clear();
        stopping = 0;
        require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "user management run failed");
    };
    run(0, json::array({message(1, 1, u8"/adduser 456 Олег"),
        message(2, 2, u8"/adduser 789 Мария Иванова"), message(3, 3, u8"➕ Добавить пользователя"),
        message(4, 4, u8"790 Алексей 🚀"), message(5, 5, "/adduser 791"), message(6, 6, "/users")}));
    require(replies.size() == 6, "each user action receives one response");
    require(replies.back().at("text").get<string>().find(u8"789 · Мария Иванова") != string::npos &&
            replies.back().at("text").get<string>().find(u8"790 · Алексей 🚀") != string::npos,
            "command and button additions display names alongside IDs");
    require(getJSONDataFromPath(statePath).at("user-names").at("789") == u8"Мария Иванова", "names saved before acknowledgment");
    run(7, json::array({message(7, 7, "/users")}));
    require(replies.back().at("text").get<string>().find(u8"789 · Мария Иванова") != string::npos &&
            replies.back().at("text").get<string>().find("777\n") != string::npos &&
            replies.back().at("text").get<string>().find("791\n") != string::npos,
            "names, legacy users and ID-only additions survive restart");
    auto unauthorized = message(11, 11, u8"/adduser 789 Чужое имя");
    unauthorized["message"]["chat"]["id"] = 456;
    unauthorized["message"]["from"]["id"] = 456;
    run(8, json::array({message(8, 8, u8"/adduser 456 Олег Петров"),
        message(9, 9, u8"/removeuser 789 Мария Иванова"), message(10, 10, "/users"),
        unauthorized, message(12, 12, "/users")}));
    require(replies[1].at("text").get<string>().find(u8"только положительный числовой ID") != string::npos,
            "remove command rejects names instead of deleting by a parsed prefix");
    require(replies[3].at("text") == u8"Это действие доступно только владельцу.", "ordinary users cannot change names");
    require(getJSONDataFromPath(statePath).at("user-names").at("789") == u8"Мария Иванова" &&
            getJSONDataFromPath(statePath).at("user-names").at("456") == u8"Олег Петров",
            "rename persists while rejected removal and non-owner changes have no effect");
    run(13, json::array({message(13, 13, u8"🚫 Отключить пользователя"),
        message(14, 14, u8"789 Мария Иванова"), message(15, 15, "789"), message(16, 16, u8"↩️ Отмена"),
        message(17, 17, "/removeuser 789"), message(18, 18, u8"✅ Да, отключить"), message(19, 19, "/users")}));
    require(replies[1].at("text").get<string>().find(u8"только положительный числовой ID") != string::npos &&
            replies[2].at("text").get<string>().find(u8"789 · Мария Иванова") != string::npos,
            "button removal accepts only ID and shows the name in confirmation");
    require(!getJSONDataFromPath(statePath).at("user-names").contains("789"), "confirmed removal forgets the name");
    run(20, json::array({message(20, 20, "/users"), message(21, 21, "/queries")}));
    require(replies[0].at("text").get<string>().find("\n789") == string::npos &&
            replies[0].at("text").get<string>().find(u8"456 · Олег Петров") != string::npos,
            "removed users stay removed and another user's name is retained");
    statePath = previousPath;
    expectedOffset = previousOffset;
}

void testProxyNotifications(const filesystem::path &directory) {
    const auto previousPath = statePath;
    const auto previousOffset = expectedOffset;
    const char *previousProxy = getenv("KUFAR_PROXY");
    const optional<string> originalProxy = previousProxy ? optional<string>(previousProxy) : nullopt;
    const auto setProxy = [](const optional<string> &value) {
#ifdef _WIN32
        _putenv_s("KUFAR_PROXY", value ? value->c_str() : "");
#else
        if (value) setenv("KUFAR_PROXY", value->c_str(), 1);
        else unsetenv("KUFAR_PROXY");
#endif
    };
    const auto configPath = (directory / "proxy-config.json").string();
    saveFile(configPath, json{{"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
        {"queries", json::array()}, {"recipients", {
            {{"chat-id", 456}, {"queries", {{{"tag", u8"Гиря"}}}}}
        }}, {"delays", {{"query", 0}, {"loop", 0}}}}.dump());
    const auto run = [&](const char *cacheName, const vector<int> &outcomes, bool fresh) {
        statePath = (directory / cacheName).string();
        if (fresh) saveFile(statePath, "[]");
        const auto state = getJSONDataFromPath(statePath);
        expectedOffset = state.is_object() ? state.value("telegram-update-offset", int64_t{0}) : 0;
        const auto messageID = state.is_object() && state.contains("telegram-message-ids")
            ? state.at("telegram-message-ids").value("123", int64_t{0}) + 1 : 1;
        proxyOutcomes = outcomes;
        proxyAlertAttempts.clear();
        batches.assign(outcomes.size() + 1, json::array());
        batches[0] = json::array({message(expectedOffset, messageID, "/menu")});
        nextBatch = 0;
        scheduleTest = proxyTest = true;
        replies.clear();
        stopping = 0;
        vector<string> arguments = {"offline-test", "--config=" + configPath, "--cache=" + statePath};
        vector<char *> argv;
        for (auto &argument : arguments) argv.push_back(argument.data());
        require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "proxy alert run failed");
        scheduleTest = proxyTest = false;
    };
    setProxy(string("socks4://offline-proxy.invalid:4153"));
    run("proxy-cache.json", {1, 1, 1, 1, 0, 1, 1, 1, 1}, true);
    require(proxyAlertAttempts == vector<size_t>({2, 7}), "alert on third failed search, suppress repeats, reset after recovery");
    require(getJSONDataFromPath(statePath).at("proxy-health").at("notified") == true, "outage acknowledgment persisted");
    run("proxy-cache.json", {1, 1, 1}, false);
    require(proxyAlertAttempts.empty(), "restart during the same outage must not resend its alert");
    run("proxy-http-cache.json", {1, 1, 2, 1, 1}, true);
    require(proxyAlertAttempts.empty(), "Kufar HTTP 403 does not count as proxy unavailability");
    run("proxy-recovery-cache.json", {1, 1, 0, 1, 1, 0, 1, 1}, true);
    require(proxyAlertAttempts.empty(), "a search succeeding through any route resets the outage counter");
    proxyAlertSendFailures = 1;
    run("proxy-send-cache.json", {1, 1, 1, 1, 1}, true);
    require(proxyAlertAttempts == vector<size_t>({2, 3}) && proxyAlertSendFailures == 0 &&
            getJSONDataFromPath(statePath).at("proxy-health").at("notified") == true,
            "failed Telegram alert is retried and only acknowledged on confirmed delivery");
    setProxy(nullopt);
    run("direct-cache.json", {1, 1, 1, 1}, true);
    require(proxyAlertAttempts.empty(), "no proxy setting means no proxy notifications");
    setProxy(originalProxy);
    nextBatch = 0;
    replies.clear();
    statePath = previousPath;
    expectedOffset = previousOffset;
}

int main() try {
    const auto directory = filesystem::temp_directory_path() /
        ("kufar-menu-test-" + to_string(chrono::system_clock::now().time_since_epoch().count()));
    filesystem::create_directory(directory);
    testUserManagement(directory);
    testProxyNotifications(directory);
    statePath = (directory / "cache.json").string();
    const auto configPath = (directory / "config.json").string();
    saveFile(statePath, "[]");
    saveFile(configPath, json{{"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
                             {"queries", json::array()}}.dump());
    vector<string> arguments = {"offline-test", "--config=" + configPath, "--cache=" + statePath};
    vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    incoming = json::array({
        message(1, 100, "/menu"), message(1, 100, "/menu"), message(2, 100, "/menu"),
        message(3, 101, "/add"), message(4, 102, u8"Гиря"),
        message(5, 103, u8"⬜ Все категории"), message(6, 103, u8"⬜ Все категории"),
        message(7, 104, u8"✅ Все категории"), message(8, 105, "/queries"), message(9, 106, "/status")
    });
    stopping = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "menu run failed");
    require(replies.size() == 7, "each real menu action must produce exactly one reply, repeated deliveries none");
    require(replies[3].at("text").get<string>().find(u8"📂 Выбрано: Все категории") != string::npos,
            "category selected once despite repeated delivery");
    require(replies[4].at("text").get<string>().find(u8"Пока ничего не выбрано") != string::npos,
            "a new click can deselect the same category");
    require(replies.back().at("text").get<string>().find("2.9.7") != string::npos, "status identifies updated code");
    require(replies.back().at("text").get<string>().find(u8"ПОИСКИ НЕ НАСТРОЕНЫ") != string::npos &&
            replies.back().at("text").get<string>().find(u8"Последняя проверка: поисков нет") != string::npos,
            "an empty query list must explain that monitoring is idle");

    replies.clear();
    incoming.push_back(message(10, 107, "/menu"));
    expectedOffset = 10;
    stopping = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "restart failed");
    require(replies.size() == 1, "restart must only reply to the new action");
    require(getJSONDataFromPath(statePath).at("telegram-message-ids").at("123") == 107,
            "latest accepted message survives restart");

    // Legacy configuration can contain whitespace that the incoming-message parser trims.
    // An old keyboard must still select the group, and deletion must survive a restart.
    const json legacyConfiguration = {
        {"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
        {"queries", json::array()},
        {"recipients", {
            {{"chat-id", 123}, {"queries", {{{"tag", u8" Книги  \n"}}, {{"tag", u8"Книга"}, {"category", 4000}}}}},
            {{"chat-id", 456}, {"queries", {{{"tag", u8"Книги"}}}}}
        }}
    };
    saveFile(configPath, legacyConfiguration.dump());
    incoming = json::array({message(11, 108, "/delete"), message(12, 109, u8"🗑 1.  Книги  \n"),
                            message(13, 110, u8"✅ Да, удалить"), message(14, 111, "/queries")});
    expectedOffset = 11;
    replies.clear();
    stopping = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "legacy deletion run failed");
    require(replies.size() == 4 && replies[1].at("text").get<string>().find(u8"Точно удалить группу") != string::npos,
            "old whitespace-padded button must select the intended query instead of being rejected");
    require(replies.back().at("text") == u8"У вас пока нет запросов.", "all variants of the user's group removed");
    require(getJSONDataFromPath(statePath).at("query-overrides").at("123").empty(),
            "empty override prevents deleted legacy configuration queries from returning");
    require(getJSONDataFromPath(configPath) == legacyConfiguration, "deletion must not modify other users' source configuration");

    auto otherUser = message(16, 75, "/queries");
    otherUser["message"]["chat"]["id"] = 456;
    otherUser["message"]["from"]["id"] = 456;
    incoming = json::array({message(15, 112, "/queries"), otherUser});
    expectedOffset = 15;
    replies.clear();
    stopping = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "restart after deletion failed");
    require(replies.size() == 2 && replies[0].at("chat_id") == 123 &&
            replies[0].at("text") == u8"У вас пока нет запросов.", "deleted group must remain absent after restart");
    require(replies[1].at("chat_id") == 456 && replies[1].at("text").get<string>().find(u8"Книги") != string::npos,
            "same query belonging to another user must survive deletion and restart");

    // Add a search during a 30-minute idle wait. It must run before the next poll,
    // prime old results silently, then deliver just the new result on a later check.
    statePath = (directory / "schedule-cache.json").string();
    saveFile(statePath, "[]");
    saveFile(configPath, json{{"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
                             {"queries", json::array()}, {"delays", {{"query", 0}, {"loop", 1800}}}}.dump());
    arguments[2] = "--cache=" + statePath;
    argv.clear();
    for (auto &argument : arguments) argv.push_back(argument.data());
    batches = {
        json::array(),
        json::array({message(1, 1, "/add"), message(2, 2, u8"Гиря"), message(3, 3, u8"⬜ Все категории"),
                     message(4, 4, u8"✅ Сохранить поиск (1)"), message(5, 5, "/status")}),
        json::array({message(6, 6, "/check"), message(6, 6, "/check"), message(9, 6, "/check")}),
        json::array({message(10, 10, "/status")})
    };
    expectedOffset = 0;
    replies.clear();
    scheduleTest = true;
    stopping = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "schedule run failed");
    scheduleTest = false;
    require(searchCount == 2, "adding a query and manual check must wake the cycle rather than wait 30 minutes");
    require(replies[4].at("text").get<string>().find(u8"ОЖИДАНИЕ ПЕРВОЙ ПРОВЕРКИ") != string::npos,
            "status before the first search must describe pending initialization");
    size_t advertsSent = 0;
    for (const auto &reply : replies) {
        if (!reply.contains("parse_mode")) continue;
        ++advertsSent;
        require(reply.at("text").get<string>().find(u8"Гиря новая") != string::npos,
                "priming and previously seen adverts must stay silent");
    }
    require(advertsSent == 1, "only one new advert must be delivered");
    require(replies.back().at("text").get<string>().find(u8"уже в кеше: 1") != string::npos &&
            replies.back().at("text").get<string>().find(u8"уведомлений: 1") != string::npos,
            "status must distinguish previously seen listings from delivered notifications");
    const auto checkedState = getJSONDataFromPath(statePath);
    require(checkedState.at("last-successful-checks").at("123").get<int64_t>() > 0,
            "last successful check must be stored on disk");
    incoming = json::array({message(11, 11, "/status")});
    expectedOffset = 11;
    replies.clear();
    stopping = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "restart with check history failed");
    require(replies.size() == 1 && replies[0].at("text").get<string>().find(u8"Последняя проверка: только что") != string::npos,
            "restarting must retain the successful check instead of returning to pending status");

    // Failed searches must not prime an empty cache, and failed sends must be retried.
    statePath = (directory / "retry-cache.json").string();
    saveFile(statePath, "[]");
    saveFile(configPath, json{{"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
        {"queries", {{{"tag", u8"Гиря"}, {"sort-type", 1}}}},
        {"delays", {{"query", 0}, {"loop", 0}}}}.dump());
    arguments[2] = "--cache=" + statePath;
    argv.clear();
    for (auto &argument : arguments) argv.push_back(argument.data());
    batches = {json::array(), json::array({message(1, 1, "/status")}),
        json::array({message(2, 2, "/status")}), json::array({message(3, 3, "/status")}),
        json::array({message(4, 4, "/status")})};
    nextBatch = 0;
    expectedOffset = 0;
    searchCount = 0;
    searchFailuresRemaining = 2;
    sendFailuresRemaining = 1;
    scheduleTest = true;
    stopping = 0;
    replies.clear();
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "retry run failed");
    scheduleTest = false;
    require(searchCount == 3 && sendFailuresRemaining == 0, "API recovery, baseline and failed-send retry must run");
    require(replies[0].at("text").get<string>().find(u8"ОШИБКА ПОИСКА") != string::npos,
            "a completely failed search must report an error before any successful priming");
    require(replies[2].at("text").get<string>().find(u8"ошибок отправки: 1") != string::npos,
            "status must distinguish Telegram send failure from an empty search");
    advertsSent = 0;
    for (const auto &reply : replies) if (reply.contains("parse_mode")) ++advertsSent;
    require(advertsSent == 1, "a failed new listing must be delivered once on retry, baseline stays silent");
    const auto retryState = getJSONDataFromPath(statePath);
    require(retryState.at("recipients").at("123").at("viewed-ads").size() == 2 &&
            retryState.at("recipients").at("123").at("initialized-queries").size() == 1,
            "successful recovery must persist baseline and confirmed delivery");

    // The first query is new and silently primes ID 502. The second query was
    // already active before 502 appeared and must still deliver it to this user.
    statePath = (directory / "overlap-cache.json").string();
    saveFile(statePath, json{{"recipients", {{"123", {{"viewed-ads", {501}},
        {"initialized-queries", {"active"}}}}}}}.dump());
    saveFile(configPath, json{{"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
        {"queries", {{{"tag", u8"Гиря"}, {"cache-key", "new"}},
                     {{"tag", u8"Гиря"}, {"cache-key", "active"}}}},
        {"delays", {{"query", 0}, {"loop", 0}}}}.dump());
    arguments[2] = "--cache=" + statePath;
    argv.clear();
    for (auto &argument : arguments) argv.push_back(argument.data());
    batches = {json::array(), json::array(), json::array({message(1, 1, "/status")})};
    nextBatch = 0;
    expectedOffset = 0;
    searchCount = 0;
    overlappingSearches = true;
    scheduleTest = true;
    stopping = 0;
    replies.clear();
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "overlapping search run failed");
    scheduleTest = false;
    advertsSent = 0;
    for (const auto &reply : replies) if (reply.contains("parse_mode")) ++advertsSent;
    require(searchCount == 2 && advertsSent == 1,
            "new query's silent baseline must not suppress a new advert in an already active search");
    require(getJSONDataFromPath(statePath).at("recipients").at("123").at("viewed-ads").size() == 2,
            "delivering a silently primed advert must not duplicate its global ID");
    batches = {json::array(), json::array(), json::array({message(2, 2, "/status")})};
    nextBatch = 0;
    expectedOffset = 2;
    scheduleTest = true;
    stopping = 0;
    replies.clear();
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "overlap restart failed");
    scheduleTest = false;
    for (const auto &reply : replies) require(!reply.contains("parse_mode"),
            "confirmed delivery must stay suppressed in both searches after a restart");
    overlappingSearches = false;

    const auto firstCategory = makeSubscription(123, {{"tag", u8"Гиря"}, {"category", 4000}});
    const auto secondCategory = makeSubscription(123, {{"tag", u8"Гиря"}, {"category", 5000}});
    require(firstCategory.cacheKey != secondCategory.cacheKey,
            "legacy config queries with the same tag must have separate category baselines");

    statePath = (directory / "legacy-category-cache.json").string();
    saveFile(statePath, json{{"recipients", {{"123", {{"viewed-ads", {501}},
        {"initialized-queries", {u8"Гиря"}}}}}}}.dump());
    saveFile(configPath, json{{"telegram", {{"bot-token", "offline-test-token"}, {"chat-id", 123}}},
        {"queries", {firstCategory.sourceQuery, secondCategory.sourceQuery}},
        {"delays", {{"query", 0}, {"loop", 0}}}}.dump());
    arguments[2] = "--cache=" + statePath;
    argv.clear();
    for (auto &argument : arguments) argv.push_back(argument.data());
    batches = {json::array(), json::array(), json::array({message(1, 1, "/status")})};
    nextBatch = 0;
    expectedOffset = 0;
    searchCount = 0;
    overlappingSearches = true;
    scheduleTest = true;
    stopping = 0;
    replies.clear();
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "legacy baseline migration failed");
    scheduleTest = false;
    overlappingSearches = false;
    advertsSent = 0;
    for (const auto &reply : replies) if (reply.contains("parse_mode")) ++advertsSent;
    require(advertsSent == 1 && getJSONDataFromPath(statePath).at("recipients").at("123").at("initialized-queries").size() == 2,
            "legacy initialized tag must migrate to distinct category keys without silently swallowing new adverts");

    // Independent diagnostic mode can run beside a live worker holding the state lock.
    StateLock heldLock(statePath);
    const string unchangedState = getTextFromFile(statePath);
    vector<string> probeArguments = {"offline-test", "--check-kufar"};
    argv.clear();
    for (auto &argument : probeArguments) argv.push_back(argument.data());
    probeTest = true;
    searchCount = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0,
            "probe must run without needing configuration, Telegram token or exclusive cache lock");
    probeTest = false;
    require(searchCount == 2 && getTextFromFile(statePath) == unchangedState,
            "probe must test both APIs without changing existing state");
    probeTest = true;
    probeFailure = true;
    searchCount = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 2,
            "probe must return a failing exit code when both APIs refuse access");
    probeTest = false;
    require(searchCount == 2 && getTextFromFile(statePath) == unchangedState,
            "even a failed probe must leave state unchanged");
    filesystem::remove_all(directory);
    cout << "Offline menu reply tests passed\n";
    return 0;
} catch (const exception &error) {
    cerr << error.what() << '\n';
    return 1;
}
