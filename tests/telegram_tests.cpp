#include "telegram.hpp"
#include "networking.hpp"
#include "json.hpp"
#include <iostream>
#include <stdexcept>

using nlohmann::json;
namespace {
std::vector<json> requests;
std::string updates;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
bool hasCommand(const json &request, const std::string &name) {
    for (const auto &command : request.at("commands"))
        if (command.at("command") == name) return true;
    return false;
}
}

// This test never contacts Telegram. It validates the outgoing API payloads.
namespace Networking {
std::string postJSONToURL(const std::string &, const std::string &body) {
    requests.push_back(json::parse(body));
    return R"({"ok":true,"result":true})";
}
std::string getJSONFromURL(const std::string &) { return updates; }
std::string postDocumentToURL(const std::string &, int64_t, const std::string &,
                             const std::string &, const std::string &) {
    throw std::runtime_error("Unexpected document upload in offline test");
}
}

int main() try {
    Telegram::setBotCommands("offline-test-token", 123);
    require(requests.size() == 2, "default and owner command menus must be registered");
    require(!requests[0].contains("scope") && hasCommand(requests[0], "id"), "everyone can discover their ID");
    for (const auto *command : {"users", "adduser", "removeuser", "backup"}) {
        require(!hasCommand(requests[0], command), "admin commands absent from default menu");
        require(hasCommand(requests[1], command), "admin commands present in owner menu");
    }
    require(requests[1].at("scope") == json{{"type", "chat"}, {"chat_id", 123}}, "admin menu scoped to owner chat");

    updates = R"({"ok":true,"result":[
        {"update_id":1,"message":{"chat":{"id":123,"type":"private"},"from":{"id":123},"text":"/users"}},
        {"update_id":2,"message":{"chat":{"id":-42,"type":"group"},"from":{"id":123},"text":"/adduser 456"}},
        {"update_id":3,"message":{"chat":{"id":123,"type":"private"},"text":"/removeuser 456"}},
        {"update_id":4,"message":{"chat":{"id":123,"type":"private"},"from":{"id":456},"text":"/users"}}
    ]})";
    const auto parsed = Telegram::getUpdates("offline-test-token", 0);
    require(parsed.size() == 4, "all update offsets retained");
    require(parsed[0].privateChat && parsed[0].senderID == parsed[0].chatID, "owner identity parsed");
    require(!parsed[1].privateChat && parsed[1].chatID == -42, "group must remain identifiable as a group");
    require(parsed[2].senderID == 0, "missing sender must not inherit chat identity");
    require(parsed[3].senderID != parsed[3].chatID, "mismatched sender must not inherit chat identity");
    updates = R"({"ok":true,"result":[{"update_id":8},{"update_id":6},{"update_id":8},{"update_id":7}]})";
    const auto ordered = Telegram::getUpdates("offline-test-token", 0);
    require(ordered.size() == 4 && ordered[0].updateID == 6 && ordered[1].updateID == 7 &&
            ordered[2].updateID == 8 && ordered[3].updateID == 8,
            "updates must be ordered so accepting a newer ID cannot skip an earlier command");
    std::cout << "Telegram command and identity tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
