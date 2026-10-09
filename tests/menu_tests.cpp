#define main kufarNotifierApplicationMain
#include "../src/main.cpp"
#undef main

// Exercise the real menu loop with an offline Telegram transport.
namespace {
json incoming;
vector<json> replies;
string statePath;
int64_t expectedOffset = 0;
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
    require(url.find("/getUpdates?timeout=0&offset=" + to_string(expectedOffset)) != string::npos,
            "menu must resume from the persisted offset");
    stopping = 1; // Process this batch and exit; no sleeping or live API calls.
    return json{{"ok", true}, {"result", incoming}}.dump();
}
string getJSONFromURL(const string &, const vector<string> &) {
    throw runtime_error("Unexpected Kufar request in offline menu test");
}
string postJSONToURL(const string &url, const string &body) {
    if (url.find("/sendMessage") != string::npos) {
        const auto persisted = getJSONDataFromPath(statePath);
        require(persisted.at("telegram-update-offset").get<int64_t>() > 0 &&
                persisted.at("telegram-message-ids").at("123").get<int64_t>() > 0,
                "both Telegram identities must be saved before replying");
        replies.push_back(json::parse(body));
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
    require(replies.back().at("text").get<string>().find("2.9") != string::npos, "status identifies updated code");

    replies.clear();
    incoming.push_back(message(10, 107, "/menu"));
    expectedOffset = 10;
    stopping = 0;
    require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "restart failed");
    require(replies.size() == 1, "restart must only reply to the new action");
    require(getJSONDataFromPath(statePath).at("telegram-message-ids").at("123") == 107,
            "latest accepted message survives restart");
    filesystem::remove_all(directory);
    cout << "Offline menu reply tests passed\n";
    return 0;
} catch (const exception &error) {
    cerr << error.what() << '\n';
    return 1;
}
