//
//  kufar.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include "json.hpp"
#include "kufar.hpp"
#include "networking.hpp"
#include "helperfunctions.hpp"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <limits>
#include <array>

namespace Kufar {

    using namespace std;
    using namespace Networking;
    using nlohmann::json;

    const array<string, 2> searchEndpoints = {{
        "https://api.kufar.by/search-api/v2/search/rendered-paginated",
        "https://searchapi.kufar.by/v1/search/rendered-paginated"
    }};
    const string mobilePhoneURL = "https://api.kufar.by/search-api/v1/ads/";
    const string currentPhoneURL = "https://api.kufar.by/search-api/v2/item/";
    const string DEFAULT_MAX_PRICE = "1000000000";

    optional<string> PriceRange::joinPrice() const {
        if (!priceMin.has_value() && !priceMax.has_value()) { return nullopt; }
        
        string joinedPrice = "";
        
        if (!priceMin.has_value()) {
            joinedPrice += '0';
        } else {
            joinedPrice += to_string(priceMin.value() * 100);
        }
        
        joinedPrice = "r:" + joinedPrice + ',' + (priceMax.has_value() ? to_string(priceMax.value() * 100) : DEFAULT_MAX_PRICE);
        
        return joinedPrice;
    }
    
    string getSortTypeUrlParameter(SortType sortType) {
        switch (sortType) {
            case SortType::newest:
                return "lst.d";
            case SortType::descending:
                return "prc.d";
            case SortType::ascending:
                return "prc.a";
            default:
                // TODO: Передалать под возврат nullopt;
                return "";
        }
    }

    namespace {
        constexpr int DEMAND_CATEGORY_ID = 7040;

        string asciiLower(string value) {
            transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
                return static_cast<char>(tolower(character));
            });
            return value;
        }

        bool isCategoryKey(const string &key) {
            const string normalizedKey = asciiLower(key);
            return normalizedKey == "cat" ||
                   normalizedKey.rfind("cat_", 0) == 0 ||
                   normalizedKey.find("category") != string::npos;
        }

        bool containsDemandMarker(const json &value) {
            if (value.is_number_integer() || value.is_number_unsigned()) {
                return value.get<long long>() == DEMAND_CATEGORY_ID;
            }

            if (value.is_string()) {
                const string text = value.get<string>();
                return text == to_string(DEMAND_CATEGORY_ID) ||
                       text == "Спрос" || text == "спрос";
            }

            if (value.is_array() || value.is_object()) {
                return any_of(value.begin(), value.end(), [](const json &item) {
                    return containsDemandMarker(item);
                });
            }

            return false;
        }

        bool containsDemandCategory(const json &value) {
            if (value.is_array()) {
                return any_of(value.begin(), value.end(), [](const json &item) {
                    return containsDemandCategory(item);
                });
            }

            if (!value.is_object()) {
                return false;
            }

            for (auto item = value.begin(); item != value.end(); ++item) {
                if (isCategoryKey(item.key()) && containsDemandMarker(item.value())) {
                    return true;
                }

                if ((item.value().is_object() || item.value().is_array()) &&
                    containsDemandCategory(item.value())) {
                    return true;
                }
            }

            return false;
        }

        void insertImageURL (vector<string> &images, const string &id, const bool yams_storage) {
            if (yams_storage) {
                images.push_back("https://yams.kufar.by/api/v1/kufar-ads/images/" + id.substr(0, 2) + "/" + id + ".jpg?rule=pictures");
            }
        }
        
        void insertImageURL (vector<string> &images, const json &imageData) {
            if (!imageData.is_object()) return;
            const auto id = getOptionalValue<string>(imageData, "id");
            if (getOptionalValue<bool>(imageData, "yams_storage").value_or(false) && id) {
                insertImageURL(images, *id, true);
                return;
            }
            const auto mediaStorage = getOptionalValue<string>(imageData, "media_storage");
            const auto path = getOptionalValue<string>(imageData, "path");
            if (mediaStorage && path && !mediaStorage->empty() && !path->empty())
                images.push_back("https://" + *mediaStorage + ".kufar.by/v1/gallery/" + *path);
        }

        int parsePrice(const json &ad) {
            if (!ad.contains("price_byn") || ad.at("price_byn").is_null()) return 0;
            const auto &price = ad.at("price_byn");
            long long value;
            if (price.is_string()) {
                const auto text = price.get<string>();
                if (text.empty()) return 0;
                size_t consumed = 0;
                value = stoll(text, &consumed);
                if (consumed != text.size()) throw runtime_error("Invalid advert price");
            } else if (price.is_number_integer()) {
                value = price.get<long long>();
            } else {
                throw runtime_error("Invalid advert price type");
            }
            if (value < 0 || value > numeric_limits<int>::max()) throw runtime_error("Invalid advert price range");
            return static_cast<int>(value);
        }
        
        void addURLParameter(ostringstream &ostream, const string &parameter, const string &value, const bool encodeValue = false) {
            ostream << parameter << '=' << (encodeValue ? urlEncode(value) : value) << '&';
        }
        
        void addURLParameter(ostringstream &ostream, const string &parameter, const optional<string> &value, const bool encodeValue = false) {
            if (value.has_value()) {
                addURLParameter(ostream, parameter, value.value(), encodeValue);
            }
        }
        
        void addURLParameterBoolean(ostringstream &ostream, const string &parameter, const optional<bool> &value, const bool encodeValue = false) {
            if (value.has_value() && value.value() == true) {
                addURLParameter(ostream, parameter, to_string(value.value()), encodeValue);
            }
        }
    
        void addURLParameter(ostringstream &ostream, const string &parameter, const optional<int> &value, const bool encodeValue = false) {
            if (value.has_value()) {
                addURLParameter(ostream, parameter, to_string(value.value()), encodeValue);
            }
        }

        optional<string> parsePhoneNumber(const string &rawJSON) {
            const json response = json::parse(rawJSON);
            if (!response.contains("phone") || !response.at("phone").is_string()) {
                return nullopt;
            }

            const string phoneNumber = response.at("phone").get<string>();
            return phoneNumber.empty() ? nullopt : optional<string>(phoneNumber);
        }

        json requestSearch(size_t endpoint, const string &parameters) {
            const auto response = json::parse(getJSONFromURL(searchEndpoints.at(endpoint) + "?" + parameters,
                {"Accept: application/json", "User-Agent: Kufar-Telegram-Notifier/2.9.5"}));
            if (!response.is_object() || !response.contains("ads") || !response.at("ads").is_array())
                throw runtime_error("Kufar search returned an invalid ads array");
            return response;
        }

        string searchFailure(const exception &error) {
            const auto *http = dynamic_cast<const HTTPError *>(&error);
            if (http && http->status() == 403 &&
                http->responseBody().find(u8"локации, где доступ") != string::npos)
                return u8"HTTP 403: Kufar сообщает об ограничении доступа из локации сервера";
            // Never print arbitrary HTTP bodies: they can contain private data or HTML.
            if (http) return http->what();
            if (dynamic_cast<const json::exception *>(&error)) return "Invalid Kufar JSON response";
            return error.what();
        }

        json searchWithFallback(const string &parameters) {
            // Keep using a working endpoint instead of retrying the rejected one for every query.
            static size_t preferredEndpoint = 0;
            vector<string> failures;
            for (size_t offset = 0; offset < searchEndpoints.size(); ++offset) {
                const size_t endpoint = (preferredEndpoint + offset) % searchEndpoints.size();
                try {
                    auto response = requestSearch(endpoint, parameters);
                    preferredEndpoint = endpoint;
                    return response;
                } catch (const exception &error) {
                    failures.push_back(searchEndpoints[endpoint] + ": " + searchFailure(error));
                    cerr << "[KUFAR API]: " << failures.back() << endl;
                }
            }
            ostringstream message;
            message << u8"Kufar: оба адреса поиска недоступны. ";
            for (const auto &failure : failures) message << failure << "; ";
            throw runtime_error(message.str());
        }
    
    }

    vector<SearchAccessResult> checkSearchAccess() {
        vector<SearchAccessResult> results;
        const string parameters = "query=" + urlEncode(u8"книга") + "&size=1&sort=lst.d";
        for (size_t endpoint = 0; endpoint < searchEndpoints.size(); ++endpoint) {
            SearchAccessResult result{searchEndpoints[endpoint], nullopt, {}};
            try { result.count = requestSearch(endpoint, parameters).at("ads").size(); }
            catch (const exception &error) { result.error = searchFailure(error); }
            results.push_back(result);
        }
        return results;
    }

    vector<Ad> getAds(const KufarConfiguration &configuration) {
        vector<Ad> adverts;
        ostringstream urlStream;
        
        addURLParameter(urlStream, "query", configuration.tag, true);
        addURLParameter(urlStream, "lang", configuration.language);
        if (configuration.limit && *configuration.limit <= 0) throw runtime_error("Invalid Kufar search limit");
        addURLParameter(urlStream, "size", to_string(min(configuration.limit.value_or(30), 200)));
        addURLParameter(urlStream, "prc", configuration.priceRange.joinPrice());
        addURLParameter(urlStream, "cur", configuration.currency);
        addURLParameter(urlStream, "cat", configuration.subCategory);
        addURLParameter(urlStream, "prn", configuration.category);

        addURLParameterBoolean(urlStream, "ot", configuration.onlyTitleSearch);
        addURLParameterBoolean(urlStream, "dle", configuration.kufarDeliveryRequired);
        addURLParameterBoolean(urlStream, "sde", configuration.kufarPaymentRequired);
        addURLParameterBoolean(urlStream, "hlv", configuration.kufarHalvaRequired);
        addURLParameterBoolean(urlStream, "oph", configuration.onlyWithPhotos);
        addURLParameterBoolean(urlStream, "ovi", configuration.onlyWithVideos);
        addURLParameterBoolean(urlStream, "pse", configuration.onlyWithExchangeAvailable);
        
        addURLParameter(urlStream, "sort", getSortTypeUrlParameter(configuration.sortType.value_or(SortType::newest)));
        if (configuration.condition.has_value()) { addURLParameter(urlStream, "cnd", int(configuration.condition.value())); }
        if (configuration.sellerType.has_value()) {
            addURLParameter(urlStream, "cmp", to_string(int(configuration.sellerType.value())));
        }
        if (configuration.region.has_value()) { addURLParameter(urlStream, "rgn", int(configuration.region.value())); }
        if (configuration.areas.has_value()) { addURLParameter(urlStream, "ar", "v.or:" + joinIntVector(configuration.areas.value(), ",")); }
            
        const json response = searchWithFallback(urlStream.str());
        const json &ads = response.at("ads");
        size_t skipped = 0;

        for (const auto &ad : ads) {
            try {
                Ad advert{};
                if (configuration.tag.has_value()) {
                    advert.tag = configuration.tag.value();
                }

                advert.title = ad.at("subject");
                advert.id = ad.at("ad_id");
                advert.date = timestampShift(zuluToTimestamp((string)ad.at("list_time")), 3);
                advert.price = parsePrice(ad);
                advert.phoneNumberIsVisible = !getOptionalValue<bool>(ad, "phone_hidden").value_or(true);
                if (ad.contains("phone") && ad.at("phone").is_string()) {
                    const string phoneNumber = ad.at("phone").get<string>();
                    if (!phoneNumber.empty()) advert.phoneNumber = phoneNumber;
                }
                advert.isDemand = containsDemandCategory(ad);
                advert.link = ad.at("ad_link");

                if (ad.contains("account_parameters") && ad.at("account_parameters").is_array()) {
                    for (const auto &accountParameter : ad.at("account_parameters")) {
                        if (getOptionalValue<string>(accountParameter, "p") == optional<string>("name")) {
                            advert.sellerName = getOptionalValue<string>(accountParameter, "v").value_or("");
                            break;
                        }
                    }
                }

                if (ad.contains("images") && ad.at("images").is_array()) {
                    for (const auto &image : ad.at("images")) insertImageURL(advert.images, image);
                }
                adverts.push_back(advert);
            } catch (const exception &exc) {
                ++skipped;
                cerr << "[KUFAR PARSE]: Skipping malformed advert: " << exc.what() << endl;
            }
        }
        if (!ads.empty() && adverts.empty())
            throw runtime_error("Kufar search returned only malformed adverts; query not initialized");
        if (skipped) cerr << "[KUFAR PARSE]: Skipped " << skipped << " of " << ads.size() << " adverts" << endl;
        
        return adverts;
    }

    optional<string> getPhoneNumber(
        const Ad &advert,
        const optional<string> &bearerToken
    ) {
        if (!advert.phoneNumberIsVisible) {
            return nullopt;
        }
        if (advert.phoneNumber.has_value() && !advert.phoneNumber->empty()) {
            return advert.phoneNumber;
        }

        if (bearerToken.has_value() && !bearerToken->empty()) {
            try {
                const optional<string> phoneNumber = parsePhoneNumber(
                    getJSONFromURL(
                        currentPhoneURL + to_string(advert.id) + "/phone",
                        {
                            "Authorization: Bearer " + bearerToken.value(),
                            "Origin: https://www.kufar.by",
                            "Referer: " + advert.link
                        }
                    )
                );
                if (phoneNumber.has_value()) {
                    return phoneNumber;
                }
            } catch (const exception &) {
                // Fall back to the mobile endpoint below.
            }
        }

        return parsePhoneNumber(
            getJSONFromURL(
                mobilePhoneURL + to_string(advert.id) + "/phone",
                {
                    "User-Agent: Kufar/v3.0.1/13/Android",
                    "X-App-Name: Android Kufar",
                    "X-App-Version: v3.0.1",
                    "X-Device-ID: d34ef48d-e772-4f50-9536-60d5cfbfbc1c"
                }
            )
        );
    }

    namespace EnumString {
        string sortType(SortType sortType) {
            switch (sortType) {
                case SortType::newest:
                    return "Сначала новые";
                case SortType::descending:
                    return "По убыванию";
                case SortType::ascending:
                    return "По возрастанию";
                default:
                    return "[Неизвестный тип]";
            }
        }
            
    	string category(Category _category) {
            switch (_category) {
                case Category::realEstate:
                    return "Недвижимость";
                case Category::carsAndTransport:
                    return "Автомобили и транспорт";
                case Category::householdAppliances:
                    return "Бытовая техника";
                case Category::computerEquipment:
                    return "Компьютерная техника";
                case Category::phonesAndTablets:
                    return "Телефоны и планшеты";
                case Category::electronics:
                    return "Электроника";
                case Category::womensWardrobe:
                    return "Женский гардероб";
                case Category::mensWardrobe:
                    return "Мужской гардероб";
                case Category::beautyAndHealth:
                    return "Красота и здоровье";
                case Category::allForChildrenAndMothers:
                    return "Всё для детей и мам";
                case Category::furniture:
                    return "Мебель";
                case Category::everythingForHome:
                    return "Всё для дома";
                case Category::repairAndBuilding:
                    return "Ремонт и стройка";
                case Category::garden:
                    return "Сад и огород";
                case Category::hobbiesSportsAndTourism:
                    return "Хобби, спорт и туризм";
                case Category::weddingAndHolidays:
                    return "Свадьба и праздники";
                case Category::animals:
                    return "Животные";
                case Category::readyBusinessAndEquipment:
                    return "Готовый бизнес и оборудование";
                case Category::job:
                    return "Работа";
                case Category::services:
                    return "Услуги";
                case Category::other:
                    return "Прочее";
                default:
                    return "[Неизвестная категория]";
            }
    	}
    
        string itemCondition(ItemCondition itemCondition) {
            switch (itemCondition) {
                case ItemCondition::_new:
                    return "Новое";
                case ItemCondition::used:
                    return "Б/У";
                default:
                    return "[Неизвестное состояние]";
            }
        }
    
        string sellerType(SellerType sellerType) {
            switch (sellerType) {
                case SellerType::individualPerson:
                    return "Частное лицо";
                case SellerType::company:
                    return "Компания";
                default:
                    return "[Неизвестный тип]";
            }
        }
    
        string region(Region region) {
            switch (region) {
                case Region::Brest:
                    return "Брест";
                case Region::Gomel:
                    return "Гомель";
                case Region::Grodno:
                    return "Гродно";
                case Region::Mogilev:
                    return "Могилёв";
                case Region::Minsk_Region:
                    return "Минская область";
                case Region::Vitebsk:
                    return "Витебск";
                case Region::Minsk:
                    return "Минск";
                default:
                    return "[Неизвестный регион]";
            }
        }
    
        string area(int value) {
            switch (value) {
                ///@b Минск
                case int(Areas::Minsk::Centralnyj):
                    return "Центральный";
                case int(Areas::Minsk::Sovetskij):
                    return "Советский";
                case int(Areas::Minsk::Pervomajskij):
                    return "Первомайский";
                case int(Areas::Minsk::Partizanskij):
                    return "Партизанский";
                case int(Areas::Minsk::Zavodskoj):
                    return "Заводской";
                case int(Areas::Minsk::Leninskij):
                    return "Ленинский";
                case int(Areas::Minsk::Oktyabrskij):
                    return "Октябрьский";
                case int(Areas::Minsk::Moskovskij):
                    return "Московский";
                case int(Areas::Minsk::Frunzenskij):
                    return "Фрунзенский";
                 
                ///@b Брестская область
                case int(Areas::Brest::Brest):
                    return "Брест";
                case int(Areas::Brest::Baranovichi):
                    return "Барановичи";
                case int(Areas::Brest::Bereza):
                    return "Береза";
                case int(Areas::Brest::Beloozyorsk):
                    return "Белоозёрск";
                case int(Areas::Brest::Gancevichi):
                    return "Ганцевичи";
                case int(Areas::Brest::Drogichin):
                    return "Дрогичин";
                case int(Areas::Brest::Zhabinka):
                    return "Жабинка";
                case int(Areas::Brest::Ivanovo):
                    return "Иваново";
                case int(Areas::Brest::Ivacevichi):
                    return "Иванцевичи";
                case int(Areas::Brest::Kamenec):
                    return "Каменец";
                case int(Areas::Brest::Kobrin):
                    return "Кобрин";
                case int(Areas::Brest::Luninec):
                    return "Лунинец";
                case int(Areas::Brest::Lyahovichi):
                    return "Ляховичи";
                case int(Areas::Brest::Malorita):
                    return "Малорита";
                case int(Areas::Brest::Pinsk):
                    return "Пинск";
                case int(Areas::Brest::Pruzhany):
                    return "Пружаны";
                case int(Areas::Brest::Stolin):
                    return "Столин";
                case int(Areas::Brest::Others):
                    return "Другое (Брест)";
                 
                ///@b Гомельская область
                case int(Areas::Gomel::Gomel):
                    return "Гомель";
                case int(Areas::Gomel::Bragin):
                    return "Брагин";
                case int(Areas::Gomel::BudaKoshelevo):
                    return "Буда-Кошелёво";
                case int(Areas::Gomel::Vetka):
                    return "Ветка";
                case int(Areas::Gomel::Dobrush):
                    return "Добруш";
                case int(Areas::Gomel::Elsk):
                    return "Ельск";
                case int(Areas::Gomel::Zhitkovichi):
                    return "Житковичи";
                case int(Areas::Gomel::Zhlobin):
                    return "Жлобин";
                case int(Areas::Gomel::Kalinkovichi):
                    return "Калинковичи";
                case int(Areas::Gomel::Korma):
                    return "Корма";
                case int(Areas::Gomel::Lelchicy):
                    return "Лельчицы";
                case int(Areas::Gomel::Loev):
                    return "Лоев";
                case int(Areas::Gomel::Mozyr):
                    return "Мозырь";
                case int(Areas::Gomel::Oktyabrskij):
                    return "Октябрьский";
                case int(Areas::Gomel::Narovlya):
                    return "Наровля";
                case int(Areas::Gomel::Petrikov):
                    return "Петриков";
                case int(Areas::Gomel::Rechica):
                    return "Речица";
                case int(Areas::Gomel::Rogachev):
                    return "Рогачёв";
                case int(Areas::Gomel::Svetlogorsk):
                    return "Светлогорск";
                case int(Areas::Gomel::Hojniki):
                    return "Хойники";
                case int(Areas::Gomel::Chechersk):
                    return "Чечерск";
                case int(Areas::Gomel::Others):
                    return "Другое (Гомель)";
                
                ///@b Гродненская область
                case int(Areas::Grodno::Grodno):
                    return "Гродно";
                case int(Areas::Grodno::Berezovka):
                    return "Берёзовка";
                case int(Areas::Grodno::Berestovica):
                    return "Берестовица";
                case int(Areas::Grodno::Volkovysk):
                    return "Волковыск";
                case int(Areas::Grodno::Voronovo):
                    return "Вороново";
                case int(Areas::Grodno::Dyatlovo):
                    return "Дятлово";
                case int(Areas::Grodno::Zelva):
                    return "Зельва";
                case int(Areas::Grodno::Ive):
                    return "Ивье";
                case int(Areas::Grodno::Korelichi):
                    return "Кореличи";
                case int(Areas::Grodno::Lida):
                    return "Лида";
                case int(Areas::Grodno::Mosty):
                    return "Мосты";
                case int(Areas::Grodno::Novogrudok):
                    return "Новогрудок";
                case int(Areas::Grodno::Ostrovec):
                    return "Островец";
                case int(Areas::Grodno::Oshmyany):
                    return "Ошмяны";
                case int(Areas::Grodno::Svisloch):
                    return "Свислочь";
                case int(Areas::Grodno::Skidel):
                    return "Скидель";
                case int(Areas::Grodno::Slonim):
                    return "Слоним";
                case int(Areas::Grodno::Smorgon):
                    return "Сморгонь";
                case int(Areas::Grodno::Shchuchin):
                    return "Щучин";
                case int(Areas::Grodno::Others):
                    return "Другое (Гродно)";
                
                ///@b Могилёв
                case int(Areas::Mogilev::Mogilev):
                    return "Могилёв";
                case int(Areas::Mogilev::Belynichi):
                    return "Белыничи";
                case int(Areas::Mogilev::Bobrujsk):
                    return "Бобруйск";
                case int(Areas::Mogilev::Byhov):
                    return "Быхов";
                case int(Areas::Mogilev::Glusk):
                    return "Глуск";
                case int(Areas::Mogilev::Gorki):
                    return "Горки";
                case int(Areas::Mogilev::Dribin):
                    return "Дрибин";
                case int(Areas::Mogilev::Kirovsk):
                    return "Кировск";
                case int(Areas::Mogilev::Klimovichi):
                    return "Климовичи";
                case int(Areas::Mogilev::Klichev):
                    return "Кличев";
                case int(Areas::Mogilev::Mstislavl):
                    return "Мстиславль";
                case int(Areas::Mogilev::Osipovichi):
                    return "Осиповичи";
                case int(Areas::Mogilev::Slavgorod):
                    return "Славгород";
                case int(Areas::Mogilev::Chausy):
                    return "Чаусы";
                case int(Areas::Mogilev::Cherikov):
                    return "Чериков";
                case int(Areas::Mogilev::Shklov):
                    return "Шклов";
                case int(Areas::Mogilev::Hotimsk):
                    return "Хотимск";
                case int(Areas::Mogilev::Others):
                    return "Другое (Могилёв)";
                
               ///@b Минская область
               case int(Areas::MinskRegion::MinskRegion):
                   return "Минский район";
               case int(Areas::MinskRegion::Berezino):
                   return "Березино";
               case int(Areas::MinskRegion::Borisov):
                   return "Борисов";
               case int(Areas::MinskRegion::Vilejka):
                   return "Вилейка";
               case int(Areas::MinskRegion::Volozhin):
                   return "Воложин";
               case int(Areas::MinskRegion::Dzerzhinsk):
                   return "Дзержинск";
               case int(Areas::MinskRegion::Zhodino):
                   return "Жодино";
               case int(Areas::MinskRegion::Zaslavl):
                   return "Заславль";
               case int(Areas::MinskRegion::Kleck):
                   return "Клецк";
               case int(Areas::MinskRegion::Kopyl):
                   return "Копыль";
               case int(Areas::MinskRegion::Krupki):
                   return "Крупки";
               case int(Areas::MinskRegion::Logojsk):
                   return "Логойск";
               case int(Areas::MinskRegion::Lyuban):
                   return "Люба💔нь";
               case int(Areas::MinskRegion::MarinaGorka):
                   return "Марьина Горка";
               case int(Areas::MinskRegion::Molodechno):
                   return "Молодечно";
               case int(Areas::MinskRegion::Myadel):
                   return "Мядель";
               case int(Areas::MinskRegion::Nesvizh):
                   return "Несвиж";
               case int(Areas::MinskRegion::Rudensk):
                   return "Руденск";
               case int(Areas::MinskRegion::Sluck):
                   return "Слуцк";
               case int(Areas::MinskRegion::Smolevichi):
                   return "Смолевичи";
               case int(Areas::MinskRegion::Soligorsk):
                   return "Солигорск";
               case int(Areas::MinskRegion::StaryeDorogi):
                   return "Старые Дороги";
               case int(Areas::MinskRegion::Stolbcy):
                   return "Столбцы";
               case int(Areas::MinskRegion::Uzda):
                   return "Узда";
               case int(Areas::MinskRegion::Fanipol):
                   return "Фаниполь";
               case int(Areas::MinskRegion::Cherven):
                   return "Червень";
               case int(Areas::MinskRegion::Others):
                   return "Другое (Минская область)";
               
               ///@b Витебская область
               case int(Areas::Vitebsk::Vitebsk):
                   return "Витбеск";
               case int(Areas::Vitebsk::Beshenkovichi):
                   return "Бешенковичи";
               case int(Areas::Vitebsk::Baran):
                   return "Барань";
               case int(Areas::Vitebsk::Braslav):
                   return "Браслав";
               case int(Areas::Vitebsk::Verhnedvinsk):
                   return "Верхнедвинск";
               case int(Areas::Vitebsk::Glubokoe):
                   return "Глубокое";
               case int(Areas::Vitebsk::Gorodok):
                   return "Городок";
               case int(Areas::Vitebsk::Dokshicy):
                   return "Докшицы";
               case int(Areas::Vitebsk::Dubrovno):
                   return "Дубровно";
               case int(Areas::Vitebsk::Lepel):
                   return "Лепель";
               case int(Areas::Vitebsk::Liozno):
                   return "Лиозно";
               case int(Areas::Vitebsk::Miory):
                   return "Миоры";
               case int(Areas::Vitebsk::Novolukoml):
                   return "Новолукомль";
               case int(Areas::Vitebsk::Novopolock):
                   return "Новополоцк";
               case int(Areas::Vitebsk::Orsha):
                   return "Орша";
               case int(Areas::Vitebsk::Polock):
                   return "Полоцк";
               case int(Areas::Vitebsk::Postavy):
                   return "Поставы";
               case int(Areas::Vitebsk::Rossony):
                   return "Россоны";
               case int(Areas::Vitebsk::Senno):
                   return "Сенно";
               case int(Areas::Vitebsk::Tolochin):
                   return "Толочин";
               case int(Areas::Vitebsk::Ushachi):
                   return "Ушачи";
               case int(Areas::Vitebsk::Chashniki):
                   return "Чашники";
               case int(Areas::Vitebsk::Sharkovshchina):
                   return "Шарковщина";
               case int(Areas::Vitebsk::Shumilino):
                   return "Шумилино";
               case int(Areas::Vitebsk::Others):
                   return "Другое (Витебск)";
               default:
                   return "[Неизвестный регион]";
            }
        }
    
        string subCategory(int value) {
            switch (value) {
                case int(SubCategories::RealEstate::NewBuildings):
                    return "Новостройки";
                case int(SubCategories::RealEstate::Apartments):
                    return "Квартиры";
                case int(SubCategories::RealEstate::Rooms):
                    return "Комнаты";
                case int(SubCategories::RealEstate::HousesAndCottages):
                    return "Дома и коттеджи";
                case int(SubCategories::RealEstate::GaragesAndParkingLots):
                    return "Гаражи и стоянки";
                case int(SubCategories::RealEstate::LandPlots):
                    return "Участки";
                case int(SubCategories::RealEstate::Commercial):
                    return "Коммерческая";
                case int(SubCategories::CarsAndTransport::passengerCars):
                    return "Легковые авто";
                case int(SubCategories::CarsAndTransport::trucksAndBuses):
                    return "Грузовики и автобусы";
                case int(SubCategories::CarsAndTransport::motorVehicles):
                    return "Мототехника";
                case int(SubCategories::CarsAndTransport::partsConsumables):
                    return "Запчасти, расходники";
                case int(SubCategories::CarsAndTransport::tiresWheels):
                    return "Шины, диски";
                case int(SubCategories::CarsAndTransport::accessories):
                    return "Аксессуары";
                case int(SubCategories::CarsAndTransport::agriculturalMachinery):
                    return "Сельхозтехника";
                case int(SubCategories::CarsAndTransport::specialMachinery):
                    return "Спецтехника";
                case int(SubCategories::CarsAndTransport::trailers):
                    return "Прицепы";
                case int(SubCategories::CarsAndTransport::waterTransport):
                    return "Водный транспорт";
                case int(SubCategories::CarsAndTransport::toolsAndEquipment):
                    return "Инструмент, оборудование";
                case int(SubCategories::HouseholdAppliances::kitchenAppliances):
                    return "Техника для кухни";
                case int(SubCategories::HouseholdAppliances::largeKitchenAppliances):
                    return "Крупная техника для кухни";
                case int(SubCategories::HouseholdAppliances::cleaningEquipment):
                    return "Техника для уборки";
                case int(SubCategories::HouseholdAppliances::clothingCareAndTailoring):
                    return "Уход за одеждой, пошив";
                case int(SubCategories::HouseholdAppliances::airConditioningEquipment):
                    return "Климатическая техника";
                case int(SubCategories::HouseholdAppliances::beautyAndHealthEquipment):
                    return "Техника для красоты и здоровья";
                case int(SubCategories::ComputerEquipment::laptops):
                    return "Ноутбуки";
                case int(SubCategories::ComputerEquipment::computers):
                    return "Компьютеры";
                case int(SubCategories::ComputerEquipment::monitors):
                    return "Мониторы";
                case int(SubCategories::ComputerEquipment::parts):
                    return "Комплектующие";
                case int(SubCategories::ComputerEquipment::officeEquipment):
                    return "Оргтехника";
                case int(SubCategories::ComputerEquipment::peripheryAndAccessories):
                    return "Периферия и аксессуары";
                case int(SubCategories::ComputerEquipment::networkEquipment):
                    return "Сетевое оборудование";
                case int(SubCategories::ComputerEquipment::otherComputerProducts):
                    return "Прочие компьютерные товары";
                case int(SubCategories::PhonesAndTablets::mobilePhones):
                    return "Мобильные телефоны";
                case int(SubCategories::PhonesAndTablets::partsForPhones):
                    return "Комплектующие для телефонов";
                case int(SubCategories::PhonesAndTablets::phoneAccessories):
                    return "Аксессуары для телефонов";
                case int(SubCategories::PhonesAndTablets::telephonyAndCommunication):
                    return "Телефония и связь";
                case int(SubCategories::PhonesAndTablets::tablests):
                    return "Планшеты";
                case int(SubCategories::PhonesAndTablets::graphicTablets):
                    return "Графические планшеты";
                case int(SubCategories::PhonesAndTablets::electronicBooks):
                    return "Электронные книги";
                case int(SubCategories::PhonesAndTablets::smartWatchesAndFitnessBracelets):
                    return "Умные часы и фитнес браслеты";
                case int(SubCategories::PhonesAndTablets::accessoriesForTabletsBooksWatches):
                    return "Аксессуары для планшетов, книг, часов";
                case int(SubCategories::PhonesAndTablets::headphones):
                    return "Наушники";
                case int(SubCategories::Electronics::audioEquipment):
                    return "Аудиотехника";
                case int(SubCategories::Electronics::TVAndVideoEquipment):
                    return "ТВ и видеотехника";
                case int(SubCategories::Electronics::photoEquipmentAndOptics):
                    return "Фототехника и оптика";
                case int(SubCategories::Electronics::gamesAndConsoles):
                    return "Игры и приставки";
                case int(SubCategories::WomensWardrobe::premiumClothing):
                    return "Премиум одежда 💎";
                case int(SubCategories::WomensWardrobe::womensClothing):
                    return "Женская одежда";
                case int(SubCategories::WomensWardrobe::womensShoes):
                    return "Женская обувь";
                case int(SubCategories::WomensWardrobe::womensAccessories):
                    return "Женские аксессуары";
                case int(SubCategories::WomensWardrobe::repairAndSewingClothes):
                    return "Ремонт и пошив одежды";
                case int(SubCategories::WomensWardrobe::clothesForPregnantWomen):
                    return "Одежда для беременных";
                case int(SubCategories::MensWardrobe::mensClothing):
                    return "Мужская одежда";
                case int(SubCategories::MensWardrobe::mensShoes):
                    return "Мужская обувь";
                case int(SubCategories::MensWardrobe::mensAccessories):
                    return "Мужские аксуссуары";
                case int(SubCategories::BeautyAndHealth::decorativeCosmetics):
                    return "Декоративная косметика";
                case int(SubCategories::BeautyAndHealth::careCosmetics):
                    return "Уходовая косметика";
                case int(SubCategories::BeautyAndHealth::perfumery):
                    return "Парфюмерия";
                case int(SubCategories::BeautyAndHealth::manicurePedicure):
                    return "Маникюр, педикюр";
                case int(SubCategories::BeautyAndHealth::hairProducts):
                    return "Средства для волос";
                case int(SubCategories::BeautyAndHealth::hygieneProductsDepilation):
                    return "Средства гигиены, депиляция";
                case int(SubCategories::BeautyAndHealth::eyelashesAndEyebrowsTattoo):
                    return "Ресницы и брови, татуаж";
                case int(SubCategories::BeautyAndHealth::cosmeticAccessories):
                    return "Косметические аксессуары";
                case int(SubCategories::BeautyAndHealth::medicalProducts):
                    return "Медицинские товары";
                case int(SubCategories::BeautyAndHealth::ServicesBeautyAndHealth):
                    return "Услуги: красота и здоровье";
                case int(SubCategories::AllForChildrenAndMothers::clothingUpTo1Year):
                    return "Одежда до 1 года";
                case int(SubCategories::AllForChildrenAndMothers::clothesForGirls):
                    return "Одежда для девочек";
                case int(SubCategories::AllForChildrenAndMothers::clothesForBoys):
                    return "Одежда для мальчиков";
                case int(SubCategories::AllForChildrenAndMothers::accessoriesForChildren):
                    return "Аксессуары для детей";
                case int(SubCategories::AllForChildrenAndMothers::childrensShoes):
                    return "Детская обувь";
                case int(SubCategories::AllForChildrenAndMothers::walkersDeckChairsSwings):
                    return "Ходунки, шезлонги, качели";
                case int(SubCategories::AllForChildrenAndMothers::strollers):
                    return "Коляски";
                case int(SubCategories::AllForChildrenAndMothers::carSeatsAndBoosters):
                    return "Автокресла и бустеры";
                case int(SubCategories::AllForChildrenAndMothers::feedingAndCare):
                    return "Кормление и уход";
                case int(SubCategories::AllForChildrenAndMothers::textileForChildren):
                    return "Текстиль для детей";
                case int(SubCategories::AllForChildrenAndMothers::kangarooBagsAndSlings):
                    return "Сумки-кенгуру и слинги";
                case int(SubCategories::AllForChildrenAndMothers::toysAndBooks):
                    return "Игрушки и книги";
                case int(SubCategories::AllForChildrenAndMothers::childrensTransport):
                    return "Детский транспорт";
                case int(SubCategories::AllForChildrenAndMothers::productsForMothers):
                    return "Товары для мам";
                case int(SubCategories::AllForChildrenAndMothers::otherProductsForChildren):
                    return "Прочие товары для детей";
                case int(SubCategories::AllForChildrenAndMothers::furnitureForChildren):
                    return "Детская мебель";
                case int(SubCategories::Furniture::banquetAndOttomans):
                    return "Банкетки, пуфики";
                case int(SubCategories::Furniture::hangersAndHallways):
                    return "Вешалки, прихожие";
                case int(SubCategories::Furniture::dressers):
                    return "Комоды";
                case int(SubCategories::Furniture::bedsAndMattresses):
                    return "Кровати, матрасы";
                case int(SubCategories::Furniture::kitchens):
                    return "Кухни";
                case int(SubCategories::Furniture::KitchenCorners):
                    return "Кухонные уголки";
                case int(SubCategories::Furniture::cushionedFurniture):
                    return "Мягкая мебель";
                case int(SubCategories::Furniture::shelvesRacksLockers):
                    return "Полки, стеллажи, шкафчики";
                case int(SubCategories::Furniture::sleepingHeadsets):
                    return "Спальные гарнитуры";
                case int(SubCategories::Furniture::wallsSectionsModules):
                    return "Стенки, секции, модули";
                case int(SubCategories::Furniture::tablesAndDiningGroups):
                    return "Столы и обеденные группы";
                case int(SubCategories::Furniture::chairs):
                    return "Стулья";
                case int(SubCategories::Furniture::cabinetsCupboards):
                    return "Тумбы, буфеты";
                case int(SubCategories::Furniture::wardrobes):
                    return "Шкафы";
                case int(SubCategories::Furniture::furnitureAccessoriesAndComponents):
                    return "Мебельная фурнитура и составляющие";
                case int(SubCategories::Furniture::otherFurniture):
                    return "Прочая мебель";
                case int(SubCategories::EverythingForHome::interiorItemsMirrors):
                    return "Предметы интерьера, зеркала";
                case int(SubCategories::EverythingForHome::curtainsBlindsCornices):
                    return "Шторы, жалюзи, карнизы";
                case int(SubCategories::EverythingForHome::textilesAndCarpets):
                    return "Текстиль и ковры";
                case int(SubCategories::EverythingForHome::lighting):
                    return "Освещение";
                case int(SubCategories::EverythingForHome::householdGoods):
                    return "Хозяйственные товары";
                case int(SubCategories::EverythingForHome::tablewareAndKitchenAccessories):
                    return "Посуда и кухонные аксессуары";
                case int(SubCategories::EverythingForHome::indoorPlants):
                    return "Комнатные растения";
                case int(SubCategories::EverythingForHome::householdServices):
                    return "Бытовые услуги";
                case int(SubCategories::EverythingForHome::furnitureRepair):
                    return "Ремонт мебели";
                case int(SubCategories::RepairAndBuilding::constructionTools):
                    return "Строительный инструмент";
                case int(SubCategories::RepairAndBuilding::constructionEquipment):
                    return "Строительное оборудование";
                case int(SubCategories::RepairAndBuilding::plumbingAndHeating):
                    return "Сантехника и отопление";
                case int(SubCategories::RepairAndBuilding::buildingMaterials):
                    return "Стройматериалы";
                case int(SubCategories::RepairAndBuilding::finishingMaterials):
                    return "Отделочные материалы";
                case int(SubCategories::RepairAndBuilding::windowsAndDoors):
                    return "Окна и двери";
                case int(SubCategories::RepairAndBuilding::housesLogCabinsAndStructures):
                    return "Дома, срубы и сооружения";
                case int(SubCategories::RepairAndBuilding::gatesFences):
                    return "Ворота, заборы";
                case int(SubCategories::RepairAndBuilding::powerSupply):
                    return "Электроснабжение";
                case int(SubCategories::RepairAndBuilding::personalProtectiveEquipment):
                    return "Средства индивидуальной защит";
                case int(SubCategories::RepairAndBuilding::otherForRepairAndConstruction):
                    return "Прочее для ремонта и стройки";
                case int(SubCategories::Garden::gardenFurnitureAndSwimmingPools):
                    return "Садовая мебель и бассейны";
                case int(SubCategories::Garden::barbecuesAccessoriesFuel):
                    return "Мангалы, аксессуары, топливо";
                case int(SubCategories::Garden::tillersAndCultivators):
                    return "Мотоблоки и культиваторы";
                case int(SubCategories::Garden::gardenEquipment):
                    return "Садовая техника";
                case int(SubCategories::Garden::gardenTools):
                    return "Садовый инвентарь";
                case int(SubCategories::Garden::greenhouses):
                    return "Теплицы и парники";
                case int(SubCategories::Garden::plantsSeedlingsAndSeeds):
                    return "Растения, рассада и семена";
                case int(SubCategories::Garden::fertilizersAndAgrochemicals):
                    return "Удобрения и агрохимия";
                case int(SubCategories::Garden::everythingForTheBeekeeper):
                    return "Все для пчеловода";
                case int(SubCategories::Garden::bathsHouseholdUnitsBathrooms):
                    return "Бани, хозблоки, санузлы";
                case int(SubCategories::Garden::otherForTheGarden):
                    return "Прочее для сада и огорода";
                case int(SubCategories::HobbiesSportsAndTourism::CDDVDRecords):
                    return "CD, DVD, пластинки";
                case int(SubCategories::HobbiesSportsAndTourism::antiquesAndCollections):
                    return "Антиквариат и коллекции";
                case int(SubCategories::HobbiesSportsAndTourism::tickets):
                    return "Билеты";
                case int(SubCategories::HobbiesSportsAndTourism::booksAndMagazines):
                    return "Книги и журналы";
                case int(SubCategories::HobbiesSportsAndTourism::metalDetectors):
                    return "Металлоискатели";
                case int(SubCategories::HobbiesSportsAndTourism::musicalInstruments):
                    return "Музыкальные инструменты";
                case int(SubCategories::HobbiesSportsAndTourism::boardGamesAndPuzzles):
                    return "Настольные игры и пазлы";
                case int(SubCategories::HobbiesSportsAndTourism::huntingAndFishing):
                    return "Охота и рыбалка";
                case int(SubCategories::HobbiesSportsAndTourism::touristGoods):
                    return "Туристические товары";
                case int(SubCategories::HobbiesSportsAndTourism::radioControlledModels):
                    return "Радиоуправляемые модели";
                case int(SubCategories::HobbiesSportsAndTourism::handiwork):
                    return "Рукоделие";
                case int(SubCategories::HobbiesSportsAndTourism::sportGoods):
                    return "Спорттовары";
                case int(SubCategories::HobbiesSportsAndTourism::bicycles):
                    return "Велосипеды";
                case int(SubCategories::HobbiesSportsAndTourism::electricTransport):
                    return "Электротранспорт";
                case int(SubCategories::HobbiesSportsAndTourism::touristServices):
                    return "Туристические услуги";
                case int(SubCategories::HobbiesSportsAndTourism::otherHobbiesSportsAndTourism):
                    return "Прочее в Хобби, спорт и туризм";
                case int(SubCategories::WeddingAndHolidays::weddingDresses):
                    return "Свадебные платья";
                case int(SubCategories::WeddingAndHolidays::weddingCostumes):
                    return "Свадебные костюмы";
                case int(SubCategories::WeddingAndHolidays::weddingShoes):
                    return "Свадебная обувь";
                case int(SubCategories::WeddingAndHolidays::weddingAccessories):
                    return "Свадебные аксессуары";
                case int(SubCategories::WeddingAndHolidays::giftsAndHolidayGoods):
                    return "Подарки и праздничные товары";
                case int(SubCategories::WeddingAndHolidays::carnivalCostumes):
                    return "Карнавальные костюмы";
                case int(SubCategories::WeddingAndHolidays::servicesForCelebrations):
                    return "Услуги для торжеств";
                case int(SubCategories::Animals::pets):
                    return "Домашние питомцы";
                case int(SubCategories::Animals::farmAnimals):
                    return "Сельхоз животные";
                case int(SubCategories::Animals::petProducts):
                    return "Товары для животных";
                case int(SubCategories::Animals::animalMating):
                    return "Вязка животных";
                case int(SubCategories::Animals::servicesForAnimals):
                    return "Услуги для животных";
                case int(SubCategories::ReadyBusinessAndEquipment::readyBusiness):
                    return "Готовый бизнес";
                case int(SubCategories::ReadyBusinessAndEquipment::businessEquipment):
                    return "Оборудование для бизнеса";
                case int(SubCategories::Job::vacancies):
                    return "Вакансии";
                case int(SubCategories::Job::lookingForAJob):
                    return "Ищу работу";
                case int(SubCategories::Services::servicesForCars):
                    return "Услуги для авто";
                case int(SubCategories::Services::computerServicesInternet):
                    return "Компьютерные услуги, интернет";
                case int(SubCategories::Services::nanniesAndNurses):
                    return "Няни и сиделки";
                case int(SubCategories::Services::educationalServices):
                    return "Образовательные услуги";
                case int(SubCategories::Services::translatorSecretaryServices):
                    return "Услуги переводчика, секретаря";
                case int(SubCategories::Services::transportationOfPassengersAndCargo):
                    return "Перевозки пассажиров и грузов";
                case int(SubCategories::Services::advertisingPrinting):
                    return "Реклама, полиграфия";
                case int(SubCategories::Services::constructionWorks):
                    return "Строительные работы";
                case int(SubCategories::Services::apartmentHouseRenovation):
                    return "Ремонт квартиры, дома";
                case int(SubCategories::Services::gardenLandscaping):
                    return "Сад, благоустройство";
                case int(SubCategories::Services::photoAndVideoShooting):
                    return "Фото и видеосъемка";
                case int(SubCategories::Services::legalServices):
                    return "Юридические услуги";
                case int(SubCategories::Services::otherServices):
                    return "Прочие услуги";
                case int(SubCategories::Other::lostAndFound):
                    return "Бюро находок";
                case int(SubCategories::Other::hookahs):
                    return "Кальяны";
                case int(SubCategories::Other::officeSupplies):
                    return "Канцелярские товары";
                case int(SubCategories::Other::foodProducts):
                    return "Продукты питания";
                case int(SubCategories::Other::electronicSteamGenerators):
                    return "Электронные парогенераторы";
                case int(SubCategories::Other::demand):
                    return "Спрос";
                case int(SubCategories::Other::everythingElse):
                    return "Все остальное";
                default:
                    return "[Неизвестная подкатегория]";
            }
        }
    }
};


