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
#include <set>
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
    json sourceQuery;
};

enum class MenuStep {
    idle,
    waitingForQuery,
    waitingForCategory,
    waitingForDelete,
    waitingForDeleteConfirmation
};

struct CategoryChoice {
    string button;
    string name;
    optional<Category> category;
    optional<int> subCategory;
    bool automatic = false;
};

struct MenuState {
    MenuStep step = MenuStep::idle;
    string pendingTag;
    string pendingDeleteQuery;
    vector<CategoryChoice> pendingCategories;
    bool automaticCategorySelected = false;
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

QuerySubscription makeSubscription(const int64_t chatID, const json &query) {
    return {
        chatID,
        parseKufarConfiguration(query),
        query.value("cache-key", query.value("tag", "")),
        query.value("exclude-title-groups", vector<vector<string>>{}),
        query.value("required-title-phrases", vector<string>{}),
        query
    };
}

string trimText(const string &value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return "";
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool isTelegramCommand(const string &text, const string &command) {
    return text == command || text.rfind(command + "@", 0) == 0;
}

const vector<CategoryChoice> &categoryChoices() {
    static const vector<CategoryChoice> choices = {
        {u8"✨ Подобрать автоматически", u8"Автоматически", nullopt, nullopt, true},
        {u8"🌐 Все категории", u8"Все категории", nullopt, nullopt, false},
        {u8"🏋️ Спорттовары", u8"Спорттовары", Category::hobbiesSportsAndTourism,
            int(SubCategories::HobbiesSportsAndTourism::sportGoods), false},
        {u8"🔌 Электроника", u8"Электроника", Category::electronics, nullopt, false},
        {u8"🏠 Бытовая техника", u8"Бытовая техника", Category::householdAppliances, nullopt, false},
        {u8"🛠 Ремонт и стройка", u8"Ремонт и стройка", Category::repairAndBuilding, nullopt, false},
        {u8"🛋 Всё для дома", u8"Всё для дома", Category::everythingForHome, nullopt, false},
        {u8"🚗 Авто", u8"Авто", Category::carsAndTransport, nullopt, false},
        {u8"🏢 Недвижимость", u8"Недвижимость", Category::realEstate, nullopt, false},
        {u8"🎨 Хобби и спорт", u8"Хобби и спорт", Category::hobbiesSportsAndTourism, nullopt, false},
        {u8"🏭 Оборудование", u8"Оборудование", Category::readyBusinessAndEquipment, nullopt, false}
    };
    return choices;
}

CategoryChoice automaticCategoryFor(const string &tag) {
    const string normalized = normalizeTitle(tag);
    const vector<string> sportTerms = {u8"гир", u8"гантел", u8"штанг", u8"фитнес"};
    for (const string &sportTerm : sportTerms) {
        if (normalized.find(sportTerm) != string::npos) {
            return {u8"🏋️ Спорттовары", u8"Спорттовары", Category::hobbiesSportsAndTourism,
                int(SubCategories::HobbiesSportsAndTourism::sportGoods), false};
        }
    }

    if (normalized.find(u8"гараж") != string::npos) {
        return {u8"🏢 Недвижимость", u8"Гаражи и стоянки", Category::realEstate,
            int(SubCategories::RealEstate::GaragesAndParkingLots), false};
    }

    return {u8"🌐 Все категории", u8"Все категории", nullopt, nullopt, false};
}

string categoryName(const QuerySubscription &subscription) {
    if (subscription.query.subCategory.has_value() &&
        subscription.query.subCategory.value() == int(SubCategories::HobbiesSportsAndTourism::sportGoods)) {
        return u8"Спорттовары";
    }
    if (subscription.query.subCategory.has_value() &&
        subscription.query.subCategory.value() == int(SubCategories::RealEstate::GaragesAndParkingLots)) {
        return u8"Гаражи и стоянки";
    }
    if (!subscription.query.category.has_value()) {
        return u8"Все категории";
    }

    switch (subscription.query.category.value()) {
        case Category::realEstate: return u8"Недвижимость";
        case Category::carsAndTransport: return u8"Авто";
        case Category::householdAppliances: return u8"Бытовая техника";
        case Category::computerEquipment: return u8"Компьютеры";
        case Category::phonesAndTablets: return u8"Телефоны и планшеты";
        case Category::electronics: return u8"Электроника";
        case Category::everythingForHome: return u8"Всё для дома";
        case Category::repairAndBuilding: return u8"Ремонт и стройка";
        case Category::garden: return u8"Сад";
        case Category::hobbiesSportsAndTourism: return u8"Хобби и спорт";
        case Category::readyBusinessAndEquipment: return u8"Оборудование";
        default: return u8"Другая категория";
    }
}

vector<QuerySubscription> subscriptionsForChat(
    const ProgramConfiguration &programConfiguration,
    const int64_t chatID
) {
    vector<QuerySubscription> result;
    for (const QuerySubscription &subscription : programConfiguration.subscriptions) {
        if (subscription.chatID == chatID) {
            result.push_back(subscription);
        }
    }
    return result;
}

string formatQueryList(const vector<QuerySubscription> &subscriptions) {
    if (subscriptions.empty()) {
        return u8"У вас пока нет запросов.";
    }

    ostringstream text;
    text << u8"📋 Ваши запросы: " << subscriptions.size() << "\n\n";
    for (size_t index = 0; index < subscriptions.size(); ++index) {
        text << index + 1 << ". "
             << subscriptions[index].query.tag.value_or(u8"Без названия")
             << u8"\n   Категория: " << categoryName(subscriptions[index]) << "\n";
    }
    return text.str();
}

string deleteButtonText(const size_t index, const QuerySubscription &subscription) {
    return u8"🗑 " + to_string(index + 1) + ". " +
           subscription.query.tag.value_or(u8"Без названия");
}

vector<vector<string>> mainMenuKeyboard() {
    return {
        {u8"📋 Мои запросы"},
        {u8"➕ Добавить запрос", u8"➖ Удалить запрос"},
        {u8"ℹ️ Статус"}
    };
}

bool sameCategory(const CategoryChoice &left, const CategoryChoice &right) {
    return left.category == right.category && left.subCategory == right.subCategory;
}

bool isAllCategories(const CategoryChoice &choice) {
    return !choice.automatic && !choice.category.has_value() && !choice.subCategory.has_value();
}

bool isSelectedCategory(const MenuState &menuState, const CategoryChoice &choice) {
    return any_of(
        menuState.pendingCategories.begin(),
        menuState.pendingCategories.end(),
        [&](const CategoryChoice &selected) { return sameCategory(selected, choice); }
    );
}

string categoryButtonText(const MenuState &menuState, const CategoryChoice &choice) {
    if (choice.automatic) {
        if (menuState.automaticCategorySelected && !menuState.pendingCategories.empty()) {
            return u8"✅ Авто: " + menuState.pendingCategories.front().name;
        }
        return choice.button;
    }
    return (isSelectedCategory(menuState, choice) ? u8"✅ " : u8"⬜ ") + choice.name;
}

string categoryConfirmButton(const size_t count) {
    return u8"✅ Добавить выбранные (" + to_string(count) + ")";
}

string selectedCategoriesText(const MenuState &menuState) {
    if (menuState.pendingCategories.empty()) {
        return u8"Пока ничего не выбрано.";
    }

    ostringstream text;
    for (size_t index = 0; index < menuState.pendingCategories.size(); ++index) {
        if (index > 0) {
            text << ", ";
        }
        text << menuState.pendingCategories[index].name;
    }
    return text.str();
}

vector<vector<string>> categoryKeyboard(const MenuState &menuState) {
    vector<vector<string>> keyboard;
    const vector<CategoryChoice> &choices = categoryChoices();
    for (size_t index = 0; index < choices.size(); index += 2) {
        vector<string> row = {categoryButtonText(menuState, choices[index])};
        if (index + 1 < choices.size()) {
            row.push_back(categoryButtonText(menuState, choices[index + 1]));
        }
        keyboard.push_back(row);
    }
    keyboard.push_back({categoryConfirmButton(menuState.pendingCategories.size())});
    keyboard.push_back({u8"↩️ Отмена"});
    return keyboard;
}

vector<vector<string>> deleteKeyboard(const vector<QuerySubscription> &subscriptions) {
    vector<vector<string>> keyboard;
    for (size_t index = 0; index < subscriptions.size(); index += 2) {
        vector<string> row = {deleteButtonText(index, subscriptions[index])};
        if (index + 1 < subscriptions.size()) {
            row.push_back(deleteButtonText(index + 1, subscriptions[index + 1]));
        }
        keyboard.push_back(row);
    }
    keyboard.push_back({u8"↩️ Отмена"});
    return keyboard;
}

void applyQueryOverrides(ProgramConfiguration &programConfiguration, const json &queryOverrides) {
    if (!queryOverrides.is_object()) {
        return;
    }

    for (auto item = queryOverrides.begin(); item != queryOverrides.end(); ++item) {
        const int64_t chatID = stoll(item.key());
        programConfiguration.subscriptions.erase(
            remove_if(
                programConfiguration.subscriptions.begin(),
                programConfiguration.subscriptions.end(),
                [&](const QuerySubscription &subscription) { return subscription.chatID == chatID; }
            ),
            programConfiguration.subscriptions.end()
        );

        if (!item.value().is_array()) {
            continue;
        }
        for (const json &query : item.value()) {
            programConfiguration.subscriptions.push_back(makeSubscription(chatID, query));
        }
    }
}

void updateQueryOverride(
    json &queryOverrides,
    const ProgramConfiguration &programConfiguration,
    const int64_t chatID
) {
    json queries = json::array();
    for (const QuerySubscription &subscription : programConfiguration.subscriptions) {
        if (subscription.chatID == chatID) {
            queries.push_back(subscription.sourceQuery);
        }
    }
    queryOverrides[to_string(chatID)] = queries;
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
                programConfiguration.subscriptions.push_back(makeSubscription(chatID, query));
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

    set<int64_t> authorizedChatIDs;
    for (const QuerySubscription &subscription : programConfiguration.subscriptions) {
        authorizedChatIDs.insert(subscription.chatID);
    }

    json queryOverrides = json::object();
    if (programConfiguration.files.cache.contents.is_object()) {
        queryOverrides = programConfiguration.files.cache.contents.value("query-overrides", json::object());
    }
    applyQueryOverrides(programConfiguration, queryOverrides);

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

    try {
        setBotCommands(programConfiguration.telegramConfiguration.botToken);
    } catch (const exception &exc) {
        cerr << "[ERROR (Telegram commands)]: " << exc.what() << endl;
    }

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
            {"telegram-update-offset", telegramUpdateOffset},
            {"query-overrides", queryOverrides}
        };
        saveFile(programConfiguration.files.cache.path, cacheData.dump());
    };

    map<int64_t, time_t> lastSuccessfulCheckByChat;
    map<int64_t, MenuState> menuStates;
    for (const int64_t chatID : authorizedChatIDs) {
        recipientCaches[chatID];
    }

    const auto pollBotCommands = [&]() {
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

                if (authorizedChatIDs.find(update.chatID) == authorizedChatIDs.end()) {
                    continue;
                }

                TelegramConfiguration telegramConfiguration = programConfiguration.telegramConfiguration;
                telegramConfiguration.chatID = update.chatID;
                MenuState &menuState = menuStates[update.chatID];
                const string text = trimText(update.text);

                const auto sendMainMenu = [&](const string &message) {
                    sendTextMessageWithKeyboard(telegramConfiguration, message, mainMenuKeyboard());
                };

                if (isTelegramCommand(text, "/start") ||
                    isTelegramCommand(text, "/menu") ||
                    text == u8"🏠 Главное меню" ||
                    text == u8"↩️ Отмена") {
                    menuState = MenuState{};
                    sendMainMenu(
                        u8"👋 Главное меню\n\n"
                        u8"Здесь можно посмотреть, добавить или удалить поисковые запросы. "
                        u8"Просто нажмите нужную кнопку."
                    );
                    continue;
                }

                if (isTelegramCommand(text, "/queries") || text == u8"📋 Мои запросы") {
                    menuState = MenuState{};
                    sendTextMessageWithKeyboard(
                        telegramConfiguration,
                        formatQueryList(subscriptionsForChat(programConfiguration, update.chatID)),
                        mainMenuKeyboard()
                    );
                    continue;
                }

                if (isTelegramCommand(text, "/add") || text == u8"➕ Добавить запрос") {
                    menuState = MenuState{};
                    menuState.step = MenuStep::waitingForQuery;
                    sendTextMessageWithKeyboard(
                        telegramConfiguration,
                        u8"➕ Напишите, что нужно искать.\n\nНапример: гиря 24 кг",
                        {{u8"↩️ Отмена"}}
                    );
                    continue;
                }

                if (isTelegramCommand(text, "/delete") || text == u8"➖ Удалить запрос") {
                    menuState = MenuState{};
                    const vector<QuerySubscription> subscriptions = subscriptionsForChat(
                        programConfiguration,
                        update.chatID
                    );
                    if (subscriptions.empty()) {
                        sendMainMenu(u8"Удалять нечего — список запросов пуст.");
                    } else {
                        menuState.step = MenuStep::waitingForDelete;
                        sendTextMessageWithKeyboard(
                            telegramConfiguration,
                            u8"➖ Какой запрос удалить?\n\nНажмите на него:",
                            deleteKeyboard(subscriptions)
                        );
                    }
                    continue;
                }

                if (isStatusCommand(text) || text == u8"ℹ️ Статус") {
                    const size_t queryCount = count_if(
                        programConfiguration.subscriptions.begin(),
                        programConfiguration.subscriptions.end(),
                        [&](const QuerySubscription &subscription) {
                            return subscription.chatID == update.chatID;
                        }
                    );

                    ostringstream status;
                    status << u8"✅ Бот работает\n\n"
                       << u8"Активных запросов: " << queryCount << "\n"
                       << u8"Последняя успешная проверка: "
                       << formatElapsed(lastSuccessfulCheckByChat[update.chatID]) << "\n"
                       << u8"Объявлений в кэше: "
                       << recipientCaches[update.chatID].viewedAds.size();

                    sendTextMessageWithKeyboard(telegramConfiguration, status.str(), mainMenuKeyboard());
                    cout << "[STATUS]: Replied to chat " << update.chatID << endl;
                    continue;
                }

                if (menuState.step == MenuStep::waitingForQuery) {
                    if (text.size() < 2 || text.size() > 50) {
                        sendTextMessageWithKeyboard(
                            telegramConfiguration,
                            u8"Введите короткий запрос — до 50 символов.",
                            {{u8"↩️ Отмена"}}
                        );
                        continue;
                    }

                    menuState.pendingTag = text;
                    menuState.step = MenuStep::waitingForCategory;
                    sendTextMessageWithKeyboard(
                        telegramConfiguration,
                        u8"📂 Где искать?\n\n"
                        u8"Выберите одну или несколько категорий. Повторное нажатие снимает галочку.\n"
                        u8"«Все категории» включает широкий поиск и выбирается отдельно.\n\n"
                        u8"Выбрано: " + selectedCategoriesText(menuState),
                        categoryKeyboard(menuState)
                    );
                    continue;
                }

                if (menuState.step == MenuStep::waitingForCategory) {
                    bool categoryButtonPressed = false;
                    for (const CategoryChoice &choice : categoryChoices()) {
                        if (text != categoryButtonText(menuState, choice)) {
                            continue;
                        }

                        categoryButtonPressed = true;
                        if (choice.automatic) {
                            if (menuState.automaticCategorySelected) {
                                menuState.pendingCategories.clear();
                                menuState.automaticCategorySelected = false;
                            } else {
                                menuState.pendingCategories = {automaticCategoryFor(menuState.pendingTag)};
                                menuState.automaticCategorySelected = true;
                            }
                            break;
                        }

                        if (menuState.automaticCategorySelected) {
                            menuState.pendingCategories.clear();
                            menuState.automaticCategorySelected = false;
                        }

                        const auto selected = find_if(
                            menuState.pendingCategories.begin(),
                            menuState.pendingCategories.end(),
                            [&](const CategoryChoice &candidate) { return sameCategory(candidate, choice); }
                        );
                        if (selected != menuState.pendingCategories.end()) {
                            menuState.pendingCategories.erase(selected);
                        } else if (isAllCategories(choice)) {
                            menuState.pendingCategories = {choice};
                        } else {
                            menuState.pendingCategories.erase(
                                remove_if(
                                    menuState.pendingCategories.begin(),
                                    menuState.pendingCategories.end(),
                                    [](const CategoryChoice &candidate) { return isAllCategories(candidate); }
                                ),
                                menuState.pendingCategories.end()
                            );
                            menuState.pendingCategories.push_back(choice);
                        }
                        break;
                    }

                    if (categoryButtonPressed) {
                        sendTextMessageWithKeyboard(
                            telegramConfiguration,
                            u8"📂 Выберите ещё категории или нажмите «Добавить выбранные».\n\n"
                            u8"Выбрано: " + selectedCategoriesText(menuState),
                            categoryKeyboard(menuState)
                        );
                        continue;
                    }

                    if (text != categoryConfirmButton(menuState.pendingCategories.size())) {
                        sendTextMessageWithKeyboard(
                            telegramConfiguration,
                            u8"Выберите категории галочками, затем нажмите «Добавить выбранные».\n\n"
                            u8"Выбрано: " + selectedCategoriesText(menuState),
                            categoryKeyboard(menuState)
                        );
                        continue;
                    }

                    if (menuState.pendingCategories.empty()) {
                        sendTextMessageWithKeyboard(
                            telegramConfiguration,
                            u8"Сначала выберите хотя бы одну категорию.",
                            categoryKeyboard(menuState)
                        );
                        continue;
                    }

                    const string normalizedTag = normalizeTitle(menuState.pendingTag);
                    const string addedTag = menuState.pendingTag;
                    vector<string> addedCategories;
                    size_t duplicateCount = 0;

                    for (const CategoryChoice &selectedCategory : menuState.pendingCategories) {
                        const bool duplicate = any_of(
                            programConfiguration.subscriptions.begin(),
                            programConfiguration.subscriptions.end(),
                            [&](const QuerySubscription &subscription) {
                                return subscription.chatID == update.chatID &&
                                       subscription.query.tag.has_value() &&
                                       normalizeTitle(subscription.query.tag.value()) == normalizedTag &&
                                       subscription.query.category == selectedCategory.category &&
                                       subscription.query.subCategory == selectedCategory.subCategory;
                            }
                        );

                        if (duplicate) {
                            ++duplicateCount;
                            continue;
                        }

                        json newQuery = {
                            {"tag", menuState.pendingTag},
                            {"only-title-search", true},
                            {"limit", 30}
                        };
                        if (selectedCategory.category.has_value()) {
                            newQuery["category"] = int(selectedCategory.category.value());
                        }
                        if (selectedCategory.subCategory.has_value()) {
                            newQuery["sub-category"] = selectedCategory.subCategory.value();
                        }

                        const string cacheKey = "telegram|" + normalizedTag + "|" +
                            (selectedCategory.category.has_value()
                                ? to_string(int(selectedCategory.category.value()))
                                : "all") + "|" +
                            (selectedCategory.subCategory.has_value()
                                ? to_string(selectedCategory.subCategory.value())
                                : "all");
                        newQuery["cache-key"] = cacheKey;

                        programConfiguration.subscriptions.push_back(makeSubscription(update.chatID, newQuery));
                        addedCategories.push_back(selectedCategory.name);
                    }

                    if (!addedCategories.empty()) {
                        recipientCaches[update.chatID];
                        updateQueryOverride(queryOverrides, programConfiguration, update.chatID);
                        saveCache();
                    }

                    menuState = MenuState{};
                    if (addedCategories.empty()) {
                        sendMainMenu(u8"Все выбранные сочетания запроса и категорий уже существуют.");
                        continue;
                    }

                    ostringstream resultMessage;
                    resultMessage << u8"✅ Запрос добавлен\n\n🔎 " << addedTag
                                  << u8"\n📂 Категорий: " << addedCategories.size() << "\n";
                    for (const string &category : addedCategories) {
                        resultMessage << u8"• " << category << "\n";
                    }
                    if (duplicateCount > 0) {
                        resultMessage << u8"\nУже существующих сочетаний пропущено: " << duplicateCount << "\n";
                    }
                    resultMessage << u8"\nСтарые объявления будут запомнены без рассылки; придут только новые.";
                    sendMainMenu(
                        resultMessage.str()
                    );
                    cout << "[MENU]: Added " << addedCategories.size()
                         << " query categories for chat " << update.chatID << endl;
                    continue;
                }

                if (menuState.step == MenuStep::waitingForDelete) {
                    const vector<QuerySubscription> subscriptions = subscriptionsForChat(
                        programConfiguration,
                        update.chatID
                    );
                    optional<size_t> selectedIndex;
                    for (size_t index = 0; index < subscriptions.size(); ++index) {
                        if (text == deleteButtonText(index, subscriptions[index])) {
                            selectedIndex = index;
                            break;
                        }
                    }

                    if (!selectedIndex.has_value()) {
                        sendTextMessageWithKeyboard(
                            telegramConfiguration,
                            u8"Нажмите на запрос, который нужно удалить.",
                            deleteKeyboard(subscriptions)
                        );
                        continue;
                    }

                    const QuerySubscription &selected = subscriptions[selectedIndex.value()];
                    menuState.pendingDeleteQuery = selected.sourceQuery.dump();
                    menuState.step = MenuStep::waitingForDeleteConfirmation;
                    sendTextMessageWithKeyboard(
                        telegramConfiguration,
                        u8"Точно удалить запрос «" + selected.query.tag.value_or(u8"Без названия") + u8"»?",
                        {{u8"✅ Да, удалить"}, {u8"↩️ Отмена"}}
                    );
                    continue;
                }

                if (menuState.step == MenuStep::waitingForDeleteConfirmation) {
                    if (text != u8"✅ Да, удалить") {
                        sendTextMessageWithKeyboard(
                            telegramConfiguration,
                            u8"Нажмите «Да, удалить» или «Отмена».",
                            {{u8"✅ Да, удалить"}, {u8"↩️ Отмена"}}
                        );
                        continue;
                    }

                    const auto subscription = find_if(
                        programConfiguration.subscriptions.begin(),
                        programConfiguration.subscriptions.end(),
                        [&](const QuerySubscription &candidate) {
                            return candidate.chatID == update.chatID &&
                                   candidate.sourceQuery.dump() == menuState.pendingDeleteQuery;
                        }
                    );

                    if (subscription == programConfiguration.subscriptions.end()) {
                        menuState = MenuState{};
                        sendMainMenu(u8"Этот запрос уже удалён.");
                        continue;
                    }

                    const string removedTag = subscription->query.tag.value_or(u8"Без названия");
                    programConfiguration.subscriptions.erase(subscription);
                    updateQueryOverride(queryOverrides, programConfiguration, update.chatID);
                    saveCache();
                    menuState = MenuState{};
                    sendMainMenu(u8"✅ Запрос «" + removedTag + u8"» удалён.");
                    cout << "[MENU]: Removed query for chat " << update.chatID << endl;
                    continue;
                }

                sendMainMenu(u8"Не понял сообщение. Выберите действие кнопкой ниже.");
            }

            if (offsetChanged) {
                saveCache();
            }
        } catch (const exception &exc) {
            cerr << "[ERROR (Telegram menu)]: " << exc.what() << endl;
        }
    };

    const auto sleepWithStatusPolling = [&](int seconds) {
        while (seconds > 0) {
            const int chunk = min(seconds, 5);
            sleep(chunk);
            seconds -= chunk;
            pollBotCommands();
        }
    };

    pollBotCommands();

    while (true) {
        // Menu actions can add or remove subscriptions while a cycle is running.
        // Work on a snapshot so Telegram changes take effect safely on the next cycle.
        const vector<QuerySubscription> cycleSubscriptions = programConfiguration.subscriptions;
        for (const auto &subscription : cycleSubscriptions) {
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

            pollBotCommands();
            sleepWithStatusPolling(programConfiguration.queryDelaySeconds);
        }
        sleepWithStatusPolling(programConfiguration.loopDelaySeconds);
    }
    return 0;
}
