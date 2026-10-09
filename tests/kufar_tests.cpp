#include "kufar.hpp"
#include "networking.hpp"
#include "json.hpp"
#include <iostream>
#include <stdexcept>

using nlohmann::json;
namespace {
std::string response;
std::string failure;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
json advert(int id) {
    return {{"ad_id", id}, {"subject", "Test advert"}, {"list_time", "2026-10-09T12:00:00Z"},
            {"price_byn", "12300"}, {"ad_link", "https://www.kufar.by/item/" + std::to_string(id)}};
}
void expectFailure(const json &body) {
    response = body.dump();
    bool rejected = false;
    try { Kufar::getAds({}); }
    catch (const std::exception &) { rejected = true; }
    require(rejected, "an invalid response must not initialize a query silently");
}
}

// Exercise the real search parser without contacting Kufar or Telegram.
namespace Networking {
std::string urlEncode(const std::string &value) { return value; }
std::string getJSONFromURL(const std::string &) {
    if (!failure.empty()) throw std::runtime_error(failure);
    return response;
}
std::string getJSONFromURL(const std::string &url, const std::vector<std::string> &) {
    return getJSONFromURL(url);
}
}

int main() try {
    response = json{{"ads", json::array({advert(1)})}}.dump();
    auto ads = Kufar::getAds({});
    require(ads.size() == 1 && ads[0].price == 12300, "missing optional fields must not abort search");
    require(ads[0].images.empty() && ads[0].sellerName.empty(), "missing photos and seller are allowed");
    require(!ads[0].phoneNumberIsVisible, "missing phone visibility must default to hidden");

    auto numeric = advert(2);
    numeric["price_byn"] = 7500;
    numeric["phone_hidden"] = false;
    numeric["account_parameters"] = json::array({{{"p", "name"}, {"v", "Seller"}}, {}});
    numeric["images"] = json::array({nullptr, {}, {{"yams_storage", true}, {"id", "abcdef"}}});
    auto free = advert(3);
    free["price_byn"] = nullptr;
    auto empty = advert(4);
    empty["price_byn"] = "";
    auto missing = advert(5);
    missing.erase("price_byn");
    response = json{{"ads", json::array({numeric, free, empty, missing})}}.dump();
    ads = Kufar::getAds({});
    require(ads.size() == 4 && ads[0].price == 7500 && ads[0].images.size() == 1,
            "numeric prices and incomplete image metadata must be supported");
    require(ads[0].sellerName == "Seller" && ads[0].phoneNumberIsVisible, "valid metadata preserved");
    require(ads[1].price == 0 && ads[2].price == 0 && ads[3].price == 0, "unspecified prices are negotiable");

    auto broken = advert(6);
    broken["price_byn"] = "12oops";
    response = json{{"ads", json::array({broken, numeric})}}.dump();
    ads = Kufar::getAds({});
    require(ads.size() == 1 && ads[0].id == 2, "one malformed advert must not discard valid results");
    expectFailure(json{{"ads", json::array({broken})}});
    expectFailure(json{{"error", "blocked"}});
    expectFailure(json{{"ads", nullptr}});
    response = R"({"ads":[]})";
    require(Kufar::getAds({}).empty(), "a legitimate empty result is successful");

    failure = "HTTP request returned status 403";
    bool informative = false;
    try { Kufar::getAds({}); }
    catch (const std::exception &error) {
        informative = std::string(error.what()).find("Kufar HTTP 403") != std::string::npos;
    }
    require(informative, "blocked search must report a useful error");
    std::cout << "Kufar search tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
