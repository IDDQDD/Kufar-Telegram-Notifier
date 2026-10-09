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
        if (searchCount > 1) ads.push_back(advert(502, u8"Гиря новая"));
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
        const auto persisted = getJSONDataFromPath(statePath);
        require(persisted.at("telegram-update-offset").get<int64_t>() > 0 &&
                persisted.at("telegram-message-ids").at("123").get<int64_t>() > 0,
                "both Telegram identities must be saved before replying");
        const auto request = json::parse(body);
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

int main() try {
    const auto directory = filesystem::temp_directory_path() /
        ("kufar-menu-test-" + to_string(chrono::system_clock::now().time_since_epoch().count()));
    filesystem::create_directory(directory);
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
    require(replies.back().at("text").get<string>().find("2.9.4") != string::npos, "status identifies updated code");
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
