#include "kufar.hpp"
#include "networking.hpp"
#include "json.hpp"
#include <iostream>
#include <stdexcept>

using nlohmann::json;
namespace {
std::string response;
std::string failure;
std::vector<std::string> urls;
std::map<std::string, long> endpointFailures;
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
std::string getJSONFromURL(const std::string &url) {
    urls.push_back(url);
    for (const auto &[endpoint, code] : endpointFailures)
        if (url.rfind(endpoint, 0) == 0) throw HTTPError(code, "Forbidden");
    if (!failure.empty()) throw std::runtime_error(failure);
    return response;
}
std::string getJSONFromURL(const std::string &url, const std::vector<std::string> &headers) {
    require(headers.size() == 2 && headers[0] == "Accept: application/json",
            "Kufar requests must declare JSON and identify the application");
    return getJSONFromURL(url);
}
}

int main() try {
    response = json{{"ads", json::array({advert(1)})}}.dump();
    auto ads = Kufar::getAds({});
    require(urls.back().find("size=30") != std::string::npos &&
            urls.back().find("sort=lst.d") != std::string::npos,
            "default search must request a bounded page ordered by newest");
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
        informative = std::string(error.what()).find("status 403") != std::string::npos &&
                      std::string(error.what()).find("api.kufar.by") != std::string::npos &&
                      std::string(error.what()).find("searchapi.kufar.by") != std::string::npos &&
                      std::string(error.what()).find(u8"локации") == std::string::npos;
    }
    require(informative, "blocked search must report a useful error");
    failure.clear();

    response = json{{"ads", json::array({advert(7)})}}.dump();
    const std::string modern = "https://api.kufar.by/search-api/v2/search/rendered-paginated";
    const std::string legacy = "https://searchapi.kufar.by/v1/search/rendered-paginated";
    endpointFailures[modern] = 403;
    urls.clear();
    Kufar::KufarConfiguration configured;
    configured.tag = "test";
    configured.limit = 30;
    configured.subCategory = 4010;
    configured.sellerType = Kufar::SellerType::individualPerson;
    ads = Kufar::getAds(configured);
    require(ads.size() == 1 && urls.size() == 2 && urls[0].rfind(modern, 0) == 0 &&
            urls[1].rfind(legacy, 0) == 0, "HTTP 403 must retry the same search at the other endpoint");
    require(urls[0].substr(urls[0].find('?')) == urls[1].substr(urls[1].find('?')) &&
            urls[1].find("cat=4010") != std::string::npos && urls[1].find("cmp=0") != std::string::npos,
            "fallback must preserve the search and all filters");
    urls.clear();
    Kufar::getAds(configured);
    require(urls.size() == 1 && urls[0].rfind(legacy, 0) == 0,
            "subsequent searches must reuse the working endpoint");

    endpointFailures[legacy] = 404;
    endpointFailures.erase(modern);
    urls.clear();
    require(Kufar::getAds(configured).size() == 1 && urls.size() == 2,
            "a preferred endpoint that stops working must fall back again");
    endpointFailures.clear();
    urls.clear();
    const auto probes = Kufar::checkSearchAccess();
    require(probes.size() == 2 && probes[0].count == 1 && probes[1].count == 1 && urls.size() == 2,
            "connectivity probe checks both endpoints independently");
    for (const auto &url : urls) require(url.find("size=1") != std::string::npos &&
        url.find(u8"query=книга") != std::string::npos, "probe must be a small filtered request");
    configured.sortType = Kufar::SortType::newest;
    Kufar::getAds(configured);
    require(urls.back().find("sort=lst.d") != std::string::npos, "newest sort supported");
    std::cout << "Kufar search tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
