//
//  main.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 02.06.2022.
//

#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <map>
#include <sstream>
#include <ctime>
#include <codecvt>
#include <locale>

#include "json.hpp"
#include "kufar.hpp"
#include "telegram.hpp"
#include "networking.hpp"
#include "helperfunctions.hpp"

using namespace std;
using namespace Kufar;
using namespace Telegram;
using nlohmann::json;

const string CACHE_FILE_NAME = "cached-data.json";
const string CONFIGURATION_FILE_NAME = "kufar-configuration.json";

struct ConfigurationFile {
    string path;
    json contents;
};

struct CacheFile {
    string path;
    json contents;
};

struct Files {
    ConfigurationFile configuration;
    CacheFile cache;
};

struct QuerySubscription {
    int64_t chatID;
    KufarConfiguration query;
    string cacheKey;
    vector<vector<string>> excludedTitleGroups;
    vector<string> requiredTitlePhrases;
};

struct RecipientCache {
    vector<int> viewedAds;
    vector<string> initializedQueries;
    map<string, int> adPrices;
};

struct ProgramConfiguration {
    vector<QuerySubscription> subscriptions;
    TelegramConfiguration telegramConfiguration;
    Files files;
    
    int queryDelaySeconds = 5;
    int loopDelaySeconds = 30;
};

string normalizeTitle(const string &value) {
    wstring_convert<codecvt_utf8<wchar_t>> converter;
    wstring normalized = converter.from_bytes(value);

    transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t character) {
        if (character == L'Ё' || character == L'ё') {
            return L'е';
        }
        if (character >= L'А' && character <= L'Я') {
            return static_cast<wchar_t>(character + (L'а' - L'А'));
        }
        if (character >= L'A' && character <= L'Z') {
            return static_cast<wchar_t>(character + (L'a' - L'A'));
        }
        return character;
    });

    return converter.to_bytes(normalized);
}

vector<string> splitSearchTerms(const string &value) {
    istringstream stream(normalizeTitle(value));
    vector<string> terms;
    string term;

    while (stream >> term) {
        terms.push_back(term);
    }

    return terms;
}

bool matchesMultiwordQuery(const string &title, const optional<string> &queryTag) {
    if (!queryTag.has_value()) {
        return true;
    }

    const vector<string> terms = splitSearchTerms(queryTag.value());
    if (terms.size() < 2) {
        return true;
    }

    const string normalizedTitle = normalizeTitle(title);
    const bool containsAllTerms = all_of(terms.begin(), terms.end(), [&](const string &term) {
        return normalizedTitle.find(term) != string::npos;
    });

    if (!containsAllTerms) {
        return false;
    }

    // A separated negation changes the meaning completely: "не проверял"
    // must not match a title that merely contains both words in another context.
    if (terms.front() == u8"не") {
        ostringstream exactPhrase;
        for (size_t index = 0; index < terms.size(); ++index) {
            if (index > 0) {
                exactPhrase << ' ';
            }
            exactPhrase << terms[index];
        }
        return normalizedTitle.find(exactPhrase.str()) != string::npos;
    }

    return true;
}

bool matchesExcludedTitleGroup(const string &title, const vector<vector<string>> &excludedTitleGroups) {
    if (excludedTitleGroups.empty()) {
        return false;
    }

    const string normalizedTitle = normalizeTitle(title);

    return any_of(excludedTitleGroups.begin(), excludedTitleGroups.end(), [&](const vector<string> &group) {
        return !group.empty() && all_of(group.begin(), group.end(), [&](const string &term) {
            return normalizedTitle.find(normalizeTitle(term)) != string::npos;
        });
    });
}

bool matchesRequiredTitlePhrase(const string &title, const vector<string> &requiredTitlePhrases) {
    if (requiredTitlePhrases.empty()) {
        return true;
    }

    const string normalizedTitle = normalizeTitle(title);
    return any_of(requiredTitlePhrases.begin(), requiredTitlePhrases.end(), [&](const string &phrase) {
        return !phrase.empty() && normalizedTitle.find(normalizeTitle(phrase)) != string::npos;
    });
}

bool isStatusCommand(const string &text) {
    return text == "/status" || text.rfind("/status@", 0) == 0;
}

string formatElapsed(const time_t timestamp) {
    if (timestamp <= 0) {
        return "ещё не завершена";
    }

    const time_t elapsed = max<time_t>(0, time(nullptr) - timestamp);
    if (elapsed < 10) {
        return "только что";
    }
    if (elapsed < 60) {
        return to_string(elapsed) + " сек. назад";
    }
    if (elapsed < 3600) {
        return to_string(elapsed / 60) + " мин. назад";
    }
    return to_string(elapsed / 3600) + " ч. назад";
}

KufarConfiguration parseKufarConfiguration(const json &query) {
    KufarConfiguration kufarConfiguration;

    kufarConfiguration.onlyTitleSearch = getOptionalValue<bool>(query, "only-title-search");
    kufarConfiguration.tag = getOptionalValue<string>(query, "tag");
    if (query.contains("price")) {
        json queryPriceData = query.at("price");
        kufarConfiguration.priceRange.priceMin = getOptionalValue<int>(queryPriceData, "min");
        kufarConfiguration.priceRange.priceMax = getOptionalValue<int>(queryPriceData, "max");
    }

    kufarConfiguration.language = getOptionalValue<string>(query, "language");
    kufarConfiguration.limit = getOptionalValue<int>(query, "limit");
    kufarConfiguration.currency = getOptionalValue<string>(query, "currency");
    kufarConfiguration.condition = getOptionalValue<ItemCondition>(query, "condition");
    kufarConfiguration.sellerType = getOptionalValue<SellerType>(query, "seller-type");
    kufarConfiguration.kufarDeliveryRequired = getOptionalValue<bool>(query, "kufar-delivery-required");
    kufarConfiguration.kufarPaymentRequired = getOptionalValue<bool>(query, "kufar-payment-required");
    kufarConfiguration.kufarHalvaRequired = getOptionalValue<bool>(query, "kufar-halva-required");
    kufarConfiguration.onlyWithPhotos = getOptionalValue<bool>(query, "only-with-photos");
    kufarConfiguration.onlyWithVideos = getOptionalValue<bool>(query, "only-with-videos");
    kufarConfiguration.onlyWithExchangeAvailable = getOptionalValue<bool>(query, "only-with-exchange-available");
    kufarConfiguration.sortType = getOptionalValue<SortType>(query, "sort-type");
    kufarConfiguration.category = getOptionalValue<Category>(query, "category");
    kufarConfiguration.subCategory = getOptionalValue<int>(query, "sub-category");
    kufarConfiguration.region = getOptionalValue<Region>(query, "region");
    kufarConfiguration.areas = getOptionalValue<vector<int>>(query, "areas");

    return kufarConfiguration;
}

void loadJSONConfigurationData(const json &data, ProgramConfiguration &programConfiguration) {
    {
        json telegramData = data.at("telegram");
        programConfiguration.telegramConfiguration.botToken = telegramData.at("bot-token");
        programConfiguration.telegramConfiguration.chatID = telegramData.at("chat-id");

        if (const char *botToken = getenv("TELEGRAM_BOT_TOKEN")) {
            programConfiguration.telegramConfiguration.botToken = botToken;
        }

        if (const char *chatID = getenv("TELEGRAM_CHAT_ID")) {
            programConfiguration.telegramConfiguration.chatID = stoll(chatID);
        }
    }
    {
        json queriesData = data.at("queries");

        if (const char *queriesJSON = getenv("KUFAR_QUERIES_JSON")) {
            queriesData = json::parse(queriesJSON);
        }
        
        const auto addSubscriptions = [&](const int64_t chatID, const json &recipientQueries) {
            for (const json &query : recipientQueries) {
                const string cacheKey = query.value(
                    "cache-key",
                    query.value("tag", "")
                );
                const vector<vector<string>> excludedTitleGroups = query.value(
                    "exclude-title-groups",
                    vector<vector<string>>{}
                );
                const vector<string> requiredTitlePhrases = query.value(
                    "required-title-phrases",
                    vector<string>{}
                );
                programConfiguration.subscriptions.push_back({
                    chatID,
                    parseKufarConfiguration(query),
                    cacheKey,
                    excludedTitleGroups,
                    requiredTitlePhrases
                });
            }
        };

        if (const char *recipientsJSON = getenv("KUFAR_RECIPIENTS_JSON")) {
            const json recipients = json::parse(recipientsJSON);
            for (const json &recipient : recipients) {
                addSubscriptions(recipient.at("chat-id").get<int64_t>(), recipient.at("queries"));
            }
        } else {
            addSubscriptions(programConfiguration.telegramConfiguration.chatID, queriesData);
        }
    }
    {
        if (data.contains("delays")) {
            json delaysData = data.at("delays");
            programConfiguration.queryDelaySeconds = delaysData.at("query");
            programConfiguration.loopDelaySeconds = delaysData.at("loop");
        }

        if (const char *queryDelay = getenv("KUFAR_QUERY_DELAY_SECONDS")) {
            programConfiguration.queryDelaySeconds = stoi(queryDelay);
        }

        if (const char *loopDelay = getenv("KUFAR_LOOP_DELAY_SECONDS")) {
            programConfiguration.loopDelaySeconds = stoi(loopDelay);
        }
    }
}

void printJSONConfigurationData(const ProgramConfiguration &programConfiguration) {
    map<int64_t, size_t> queryCounts;
    for (const auto &subscription : programConfiguration.subscriptions) {
        queryCounts[subscription.chatID] += 1;
    }

    cout << "[CONFIG]: Recipients=" << queryCounts.size()
         << ", queries=" << programConfiguration.subscriptions.size()
         << ", query-delay=" << programConfiguration.queryDelaySeconds << "s"
         << ", loop-delay=" << programConfiguration.loopDelaySeconds << "s" << endl;

    for (const auto &[chatID, count] : queryCounts) {
        cout << "[CONFIG]: Chat " << chatID << ", queries=" << count << endl;
    }
}

json getJSONDataFromPath(const string &JSONFilePath) {
    cout << "[Загрузка файла]: " << '"' << JSONFilePath << '"' << endl;

    if (!fileExists(JSONFilePath)){
        cout << "[ОШИБКА]: Файл не существует по данному пути или к нему нет доступа." << endl;
        exit(1);
    }
    
    if (getFileSize(JSONFilePath) > 4000000) {
        cout << "[ОШИБКА]: Размер файла превышает 4МБ." << endl;
        exit(1);
    }
        
    try {
       json textFromFile = json::parse(getTextFromFile(JSONFilePath));
       return textFromFile;
    } catch (const exception &exc) {
       cout << "[ОШИБКА]: Невозможно получить данные из файла " << '"' << JSONFilePath << '"' << endl;
       cout << "::: " << exc.what() << " :::" << endl;
       exit(1);
    }

}

const string prefixConfigurationFile = "--config=";
const string prefixCacheFile = "--cache=";

/**
  Загрузка файлов:
  kufar-configuration.json,
  cached-data.json
 */

Files getFiles(const int &argsCount, char **args) {
    Files files;
    
    for (int i = 0; i < argsCount; i++){
        string currentArgument = args[i];
        
        if(stringHasPrefix(currentArgument, prefixConfigurationFile)) {
            currentArgument.erase(0, prefixConfigurationFile.length());
            files.configuration.path = currentArgument;
        } else if (stringHasPrefix(currentArgument, prefixCacheFile)) {
            currentArgument.erase(0, prefixCacheFile.length());
            files.cache.path = currentArgument;
        }
        
    }

    if (files.configuration.path.empty()) {
        if (const char *configurationPath = getenv("KUFAR_CONFIG_PATH")) {
            files.configuration.path = configurationPath;
        }
    }

    if (files.cache.path.empty()) {
        if (const char *cachePath = getenv("KUFAR_CACHE_PATH")) {
            files.cache.path = cachePath;
        } else if (const char *volumePath = getenv("RAILWAY_VOLUME_MOUNT_PATH")) {
            files.cache.path = string(volumePath) + PATH_SEPARATOR + CACHE_FILE_NAME;
        }
    }
    
    if (files.configuration.path.empty() || files.cache.path.empty()) {
        optional<string> applicationDirectory = getWorkingDirectory();
        
        if (!applicationDirectory.has_value()) {
            cout << "[ОШИБКА]: Невозможно автоматически определить путь к текущей папке. Передайте файл конфигурации/кеша в виде аргумента." << endl;
            exit(1);
        }
        
        if (files.configuration.path.empty()) {
            files.configuration.path = applicationDirectory.value() + PATH_SEPARATOR + CONFIGURATION_FILE_NAME;
        }
        
        if (files.cache.path.empty()) {
            files.cache.path = applicationDirectory.value() + PATH_SEPARATOR + CACHE_FILE_NAME;
        }
    }
    
    files.configuration.contents = getJSONDataFromPath(files.configuration.path);

    if (!fileExists(files.cache.path)) {
        const filesystem::path cachePath(files.cache.path);
        if (cachePath.has_parent_path()) {
            filesystem::create_directories(cachePath.parent_path());
        }
        saveFile(files.cache.path, "[]");
    }

    files.cache.contents = getJSONDataFromPath(files.cache.path);
    
    return files;
}

int main(int argc, char **argv) {
    ProgramConfiguration programConfiguration;
    map<int64_t, RecipientCache> recipientCaches;
    
    programConfiguration.files = getFiles(argc, argv);
    loadJSONConfigurationData(programConfiguration.files.configuration.contents, programConfiguration);

    if (programConfiguration.telegramConfiguration.botToken.empty() ||
        programConfiguration.telegramConfiguration.botToken == "1111111111:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") {
        cerr << "[ERROR]: Set TELEGRAM_BOT_TOKEN before starting the bot." << endl;
        return 1;
    }

    if (programConfiguration.telegramConfiguration.chatID == 1111111111) {
        const optional<int64_t> discoveredChatID = getLatestChatID(
            programConfiguration.telegramConfiguration.botToken
        );

        if (discoveredChatID.has_value()) {
            cout << "[SETUP]: TELEGRAM_CHAT_ID=" << discoveredChatID.value() << endl;
            cout << "[SETUP]: Add this value to Railway and redeploy the service." << endl;
            return 0;
        }

        cerr << "[SETUP]: Send /start to your bot, then redeploy to discover TELEGRAM_CHAT_ID." << endl;
        return 1;
    }

    printJSONConfigurationData(programConfiguration);

    vector<int> legacyViewedAds;
    vector<string> legacyInitializedQueries;
    map<string, int> legacyAdPrices;
    int64_t telegramUpdateOffset = 0;
    if (programConfiguration.files.cache.contents.is_array()) {
        legacyViewedAds = programConfiguration.files.cache.contents.get<vector<int>>();
    } else {
        legacyViewedAds = programConfiguration.files.cache.contents.value("viewed-ads", vector<int>{});
        legacyInitializedQueries = programConfiguration.files.cache.contents.value("initialized-queries", vector<string>{});
        legacyAdPrices = programConfiguration.files.cache.contents.value("ad-prices", map<string, int>{});
        telegramUpdateOffset = programConfiguration.files.cache.contents.value("telegram-update-offset", int64_t{0});

        if (programConfiguration.files.cache.contents.contains("recipients")) {
            const json &recipientsCache = programConfiguration.files.cache.contents.at("recipients");
            for (auto item = recipientsCache.begin(); item != recipientsCache.end(); ++item) {
                const int64_t chatID = stoll(item.key());
                recipientCaches[chatID] = {
                    item.value().value("viewed-ads", vector<int>{}),
                    item.value().value("initialized-queries", vector<string>{}),
                    item.value().value("ad-prices", map<string, int>{})
                };
            }
        }
    }

    // Preserve the existing recipient's cache when upgrading from the single-recipient format.
    if (recipientCaches.find(programConfiguration.telegramConfiguration.chatID) == recipientCaches.end()) {
        recipientCaches[programConfiguration.telegramConfiguration.chatID] = {
            legacyViewedAds,
            legacyInitializedQueries,
            legacyAdPrices
        };
    }

    const auto saveCache = [&]() {
        json recipientsCache = json::object();
        for (const auto &[chatID, recipientCache] : recipientCaches) {
            recipientsCache[to_string(chatID)] = {
                {"viewed-ads", recipientCache.viewedAds},
                {"initialized-queries", recipientCache.initializedQueries},
                {"ad-prices", recipientCache.adPrices}
            };
        }
        json cacheData = {
            {"recipients", recipientsCache},
            {"telegram-update-offset", telegramUpdateOffset}
        };
        saveFile(programConfiguration.files.cache.path, cacheData.dump());
    };

    map<int64_t, size_t> queryCounts;
    map<int64_t, time_t> lastSuccessfulCheckByChat;
    for (const auto &subscription : programConfiguration.subscriptions) {
        queryCounts[subscription.chatID] += 1;
        recipientCaches[subscription.chatID];
    }

    const auto pollStatusCommands = [&]() {
        try {
            const vector<TelegramUpdate> updates = getUpdates(
                programConfiguration.telegramConfiguration.botToken,
                telegramUpdateOffset
            );
            bool offsetChanged = false;

            for (const TelegramUpdate &update : updates) {
                if (update.updateID >= telegramUpdateOffset) {
                    telegramUpdateOffset = update.updateID + 1;
                    offsetChanged = true;
                }

                const auto queryCount = queryCounts.find(update.chatID);
                if (!isStatusCommand(update.text) || queryCount == queryCounts.end()) {
                    continue;
                }

                TelegramConfiguration telegramConfiguration = programConfiguration.telegramConfiguration;
                telegramConfiguration.chatID = update.chatID;

                ostringstream status;
                status << "✅ Бот работает\n\n"
                       << "Активных запросов: " << queryCount->second << "\n"
                       << "Последняя успешная проверка: "
                       << formatElapsed(lastSuccessfulCheckByChat[update.chatID]) << "\n"
                       << "Объявлений в кэше: "
                       << recipientCaches[update.chatID].viewedAds.size();

                sendTextMessage(telegramConfiguration, status.str());
                cout << "[STATUS]: Replied to chat " << update.chatID << endl;
            }

            if (offsetChanged) {
                saveCache();
            }
        } catch (const exception &exc) {
            cerr << "[ERROR (Telegram status)]: " << exc.what() << endl;
        }
    };

    const auto sleepWithStatusPolling = [&](int seconds) {
        while (seconds > 0) {
            const int chunk = min(seconds, 5);
            sleep(chunk);
            seconds -= chunk;
            pollStatusCommands();
        }
    };

    pollStatusCommands();

    while (true) {
        for (const auto &subscription : programConfiguration.subscriptions) {
            unsigned int sentCount = 0;
            unsigned int filteredDemandCount = 0;
            unsigned int filteredTitleCount = 0;
            unsigned int filteredQueryTermsCount = 0;
            unsigned int filteredRequiredPhraseCount = 0;
            bool cacheChanged = false;
            const auto &requestConfiguration = subscription.query;
            const string &queryKey = subscription.cacheKey;
            RecipientCache &recipientCache = recipientCaches[subscription.chatID];
            const bool queryInitialized = find(
                recipientCache.initializedQueries.begin(),
                recipientCache.initializedQueries.end(),
                queryKey
            ) != recipientCache.initializedQueries.end();
            
            try {
                for (const auto &advert : getAds(requestConfiguration)) {
                    if (advert.isDemand) {
                        filteredDemandCount += 1;
                        continue;
                    }

                    if (matchesExcludedTitleGroup(advert.title, subscription.excludedTitleGroups)) {
                        filteredTitleCount += 1;
                        continue;
                    }

                    if (!matchesMultiwordQuery(advert.title, requestConfiguration.tag)) {
                        filteredQueryTermsCount += 1;
                        continue;
                    }

                    if (!matchesRequiredTitlePhrase(advert.title, subscription.requiredTitlePhrases)) {
                        filteredRequiredPhraseCount += 1;
                        continue;
                    }

                    const string advertID = to_string(advert.id);
                    if (!vectorContains(recipientCache.viewedAds, advert.id)) {
                        recipientCache.viewedAds.push_back(advert.id);
                        recipientCache.adPrices[advertID] = advert.price;
                        cacheChanged = true;

                        if (queryInitialized) {
                            cout << "[New]: Adding [Chat ID: " << subscription.chatID << "], [Title: " << advert.title << "], [ID: " << advert.id << "], [Tag: " << advert.tag << "], [Link: " << advert.link << "]" << endl;
                            sentCount += 1;

                            try {
                                TelegramConfiguration telegramConfiguration = programConfiguration.telegramConfiguration;
                                telegramConfiguration.chatID = subscription.chatID;
                                sendAdvert(telegramConfiguration, advert);
                                usleep(300000); // Keep Telegram sends gently rate-limited.
                            } catch (const exception &exc) {
                                cerr << "[ERROR (sendAdvert)]: " << exc.what() << endl;
                            }
                        }
                    } else {
                        const auto previousPrice = recipientCache.adPrices.find(advertID);
                        if (previousPrice == recipientCache.adPrices.end()) {
                            // Migration from the old cache format: establish a baseline silently.
                            recipientCache.adPrices[advertID] = advert.price;
                            cacheChanged = true;
                        } else if (previousPrice->second != advert.price) {
                            const int oldPrice = previousPrice->second;
                            previousPrice->second = advert.price;
                            cacheChanged = true;

                            if (queryInitialized && advert.price < oldPrice) {
                                cout << "[PRICE DROP]: Chat " << subscription.chatID
                                     << ", ad=" << advert.id
                                     << ", old=" << oldPrice
                                     << ", new=" << advert.price << endl;
                                sentCount += 1;

                                try {
                                    TelegramConfiguration telegramConfiguration = programConfiguration.telegramConfiguration;
                                    telegramConfiguration.chatID = subscription.chatID;
                                    sendPriceDrop(telegramConfiguration, advert, oldPrice);
                                    usleep(300000);
                                } catch (const exception &exc) {
                                    cerr << "[ERROR (sendPriceDrop)]: " << exc.what() << endl;
                                }
                            }
                        }
                    }
                }

                lastSuccessfulCheckByChat[subscription.chatID] = time(nullptr);

                if (!queryInitialized) {
                    recipientCache.initializedQueries.push_back(queryKey);
                    cacheChanged = true;
                    cout << "[CACHE]: Initial listings stored without Telegram notifications for chat " << subscription.chatID << ", query: " << queryKey << endl;
                }
            } catch (const exception &exc) {
                cerr << "[ERROR (getAds)]: " << exc.what() << endl;
            }

            if (filteredDemandCount > 0 || filteredTitleCount > 0 || filteredQueryTermsCount > 0 || filteredRequiredPhraseCount > 0) {
                cout << "[FILTER]: Chat " << subscription.chatID
                     << ", query=" << queryKey
                     << ", demand=" << filteredDemandCount
                     << ", title=" << filteredTitleCount
                     << ", terms=" << filteredQueryTermsCount
                     << ", required=" << filteredRequiredPhraseCount << endl;
            }

            if (cacheChanged || sentCount > 0) {
                saveCache();
            }

            pollStatusCommands();
            sleepWithStatusPolling(programConfiguration.queryDelaySeconds);
        }
        sleepWithStatusPolling(programConfiguration.loopDelaySeconds);
    }
    return 0;
}
