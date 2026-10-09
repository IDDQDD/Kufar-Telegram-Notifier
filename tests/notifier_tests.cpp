#define main kufarNotifierApplicationMain
#include "../src/main.cpp"
#undef main

#include <filesystem>
#include <stdexcept>

using namespace std;

namespace {
    void require(const bool condition, const string &message) {
        if (!condition) {
            throw runtime_error(message);
        }
    }

    void testClearAllQueries() {
        ProgramConfiguration configuration;
        configuration.access.owner = 123;
        configuration.access.initial = {123, 456, 789};
        configuration.subscriptions = {
            makeSubscription(123, {{"tag", u8"Книги"}}),
            makeSubscription(456, {{"tag", u8"Гиря"}})
        };
        const json oldState = {
            {"telegram-update-offset", 90}, {"telegram-message-ids", {{"123", 70}}},
            {"user-access", {{"777", true}, {"789", false}}},
            {"query-overrides", {{"777", {{{"tag", "old query"}}}}}},
            {"recipients", {{"456", {{"viewed-ads", {42}}, {"initialized-queries", {"old"}},
                                       {"ad-prices", {{"42", 100}}}}}}},
            {"completed-backup-request", "keep"}
        };
        const auto state = withoutQuerySubscriptions(configuration, oldState);
        for (const auto id : {123, 456, 777, 789})
            require(state.at("query-overrides").at(to_string(id)).empty(), "all users' query overrides cleared, including disabled users");
        require(state.at("telegram-update-offset") == 90 && state.at("telegram-message-ids") == oldState.at("telegram-message-ids"),
                "clearing queries must not replay processed commands");
        require(state.at("user-access") == oldState.at("user-access") && state.at("completed-backup-request") == "keep",
                "clearing queries preserves user access and backup state");
        require(state.at("recipients").at("456").at("viewed-ads") == json::array({42}) &&
                state.at("recipients").at("456").at("ad-prices") == oldState.at("recipients").at("456").at("ad-prices"),
                "clearing queries preserves advert and price history");
        require(state.at("recipients").at("456").at("initialized-queries").empty(), "removed searches no longer initialized");
        applyQueryOverrides(configuration, state.at("query-overrides"));
        require(configuration.subscriptions.empty(), "configured and menu searches must remain empty on restart");
        const auto legacy = withoutQuerySubscriptions(configuration, json::array({10, 11}));
        require(legacy.at("viewed-ads") == json::array({10, 11}), "legacy advert history preserved");

        const auto directory = filesystem::temp_directory_path() /
            ("kufar-clear-test-" + to_string(chrono::system_clock::now().time_since_epoch().count()));
        filesystem::create_directory(directory);
        const auto cachePath = directory / "cache.json";
        const auto configPath = directory / "config.json";
        saveFile(cachePath.string(), oldState.dump());
        const json rawConfiguration = {
            {"telegram", {{"bot-token", "1111111111:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}, {"chat-id", 123}}},
            {"queries", {{{"tag", "configured query"}}}},
            {"recipients", {{{"chat-id", 123}, {"queries", {{{"tag", "configured query"}}}}},
                            {{"chat-id", 456}, {"queries", json::array()}}}}
        };
        saveFile(configPath.string(), rawConfiguration.dump());
        vector<string> arguments = {"offline-test", "--config=" + configPath.string(), "--cache=" + cachePath.string(), "--clear-all-queries"};
        vector<char *> argv;
        for (auto &argument : arguments) argv.push_back(argument.data());
        require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0,
                "one-shot clear exits before token validation and never launches a live bot");
        const auto clearedOnDisk = getJSONDataFromPath(cachePath.string());
        require(clearedOnDisk.at("query-overrides").at("123").empty() && clearedOnDisk.at("telegram-update-offset") == 90,
                "one-shot command clears stored searches without losing the offset");
        require(getJSONDataFromPath(configPath.string()) == rawConfiguration, "source configuration retained for recovery");
        bool backupFound = false;
        for (const auto &entry : filesystem::directory_iterator(directory)) {
            if (entry.path().filename().string().find("cache.json.before-clear-") != 0) continue;
            require(getJSONDataFromPath(entry.path().string()) == oldState, "backup contains previous state");
#if defined(__linux__)
            require((entry.status().permissions() & filesystem::perms::group_read) == filesystem::perms::none &&
                    (entry.status().permissions() & filesystem::perms::others_read) == filesystem::perms::none,
                    "backup must be private");
#endif
            backupFound = true;
        }
        require(backupFound, "clear command creates a recovery backup");

        json ownerState = oldState;
        ownerState["recipients"]["123"] = {{"viewed-ads", {55}}, {"initialized-queries", {"owner search"}}};
        ProgramConfiguration ownerConfiguration;
        ownerConfiguration.telegramConfiguration.chatID = 123;
        ownerConfiguration.access.owner = 123;
        ownerConfiguration.access.initial = {123, 456};
        ownerConfiguration.subscriptions = {
            makeSubscription(123, {{"tag", "owner query"}}),
            makeSubscription(456, {{"tag", "other user's query"}})
        };
        const auto ownerCleared = withoutQuerySubscriptions(ownerConfiguration, ownerState, 123);
        require(ownerCleared.at("query-overrides").at("123").empty(), "owner's queries cleared");
        require(ownerCleared.at("query-overrides").at("777") == ownerState.at("query-overrides").at("777"),
                "other menu user's queries must remain unchanged");
        require(ownerCleared.at("recipients").at("456") == ownerState.at("recipients").at("456"),
                "other user's initialization and advert history must remain unchanged");
        require(ownerCleared.at("recipients").at("123").at("initialized-queries").empty() &&
                ownerCleared.at("recipients").at("123").at("viewed-ads") == json::array({55}),
                "owner's search initialization cleared while history retained");
        applyQueryOverrides(ownerConfiguration, ownerCleared.at("query-overrides"));
        require(ownerConfiguration.subscriptions.size() == 2 &&
                none_of(ownerConfiguration.subscriptions.begin(), ownerConfiguration.subscriptions.end(),
                        [](const auto &subscription) { return subscription.chatID == 123; }),
                "only owner's queries remain empty on restart; other configured and menu searches survive");
        const auto ownerCachePath = directory / "owner-cache.json";
        saveFile(ownerCachePath.string(), ownerState.dump());
        arguments[2] = "--cache=" + ownerCachePath.string();
        arguments[3] = "--clear-my-queries";
        argv.clear();
        for (auto &argument : arguments) argv.push_back(argument.data());
        require(kufarNotifierApplicationMain(static_cast<int>(argv.size()), argv.data()) == 0, "owner-only clear must exit offline");
        require(getJSONDataFromPath(ownerCachePath.string()) == ownerCleared,
                "one-shot owner command preserves all other users and Telegram offsets on disk");
        filesystem::remove_all(directory);
    }

    void testMultiwordMatching() {
        require(
            matchesMultiwordQuery(u8"Импульсный блок питания", optional<string>(u8"блок питания")),
            "multiword query must require every word"
        );
        require(
            !matchesMultiwordQuery(u8"Блок без кабеля", optional<string>(u8"блок питания")),
            "multiword query must reject a missing word"
        );
        require(
            matchesMultiwordQuery(u8"Не проверял после хранения", optional<string>(u8"не проверял")),
            "negative query must match the exact phrase"
        );
        require(
            !matchesMultiwordQuery(u8"Проверял, но не включал", optional<string>(u8"не проверял")),
            "negative query must preserve word order"
        );
    }

    void testGroupedQueriesAndDeletionKeyboard() {
        const auto paddedGroups = groupQueries({makeSubscription(123, {{"tag", u8" Книги  \n"}})});
        require(deleteButtonText(0, paddedGroups.front()) == u8"🗑 1. Книги",
                "legacy whitespace must not leak into new deletion button labels");
        const auto blankGroups = groupQueries({makeSubscription(123, {{"tag", " \t\n"}})});
        require(deleteButtonText(0, blankGroups.front()) == u8"🗑 1. Без названия",
                "legacy whitespace-only searches need a usable deletion button");
        const int64_t chatID = 123;
        vector<QuerySubscription> subscriptions = {
            makeSubscription(chatID, {
                {"tag", u8"Блок питания"},
                {"category", int(Category::electronics)}
            }),
            makeSubscription(chatID, {
                {"tag", u8"блок питания"},
                {"category", int(Category::householdAppliances)},
                {"seller-type", int(SellerType::individualPerson)}
            }),
            makeSubscription(chatID, {{"tag", u8"Осциллограф"}})
        };

        const vector<QueryDisplayGroup> groups = groupQueries(subscriptions);
        require(groups.size() == 2, "same query text must be grouped regardless of case");
        require(groups[0].searchCount == 2, "group must include all category searches");
        require(groups[0].categories.size() == 2, "group must list unique categories");

        const vector<vector<string>> keyboard = deleteKeyboard(groups);
        require(!keyboard.empty(), "delete keyboard must not be empty");
        require(keyboard[0].size() == 2, "delete keyboard must show one button per grouped query");
        require(
            keyboard[0][0].find(u8"Блок питания") != string::npos,
            "delete button must use the display query text"
        );
        require(
            keyboard[0][0].find("(2") == string::npos,
            "delete button must not show the number of category searches"
        );

        Kufar::Ad phoneAdvert;
        phoneAdvert.phoneNumberIsVisible = true;
        phoneAdvert.phoneNumber = "+375291112233";
        require(
            getPhoneNumber(phoneAdvert) == phoneAdvert.phoneNumber,
            "an already available phone number must not trigger another request"
        );
        phoneAdvert.phoneNumber.reset();
        phoneAdvert.phoneNumberIsVisible = false;
        require(
            !getPhoneNumber(phoneAdvert).has_value(),
            "a hidden phone number must never be requested"
        );

        const size_t removedCount = removeGroupedQueries(
            subscriptions,
            chatID,
            queryGroupKey(u8"Блок питания")
        );
        require(removedCount == 2, "group deletion must remove every category of a query");
        require(subscriptions.size() == 1, "group deletion must preserve other queries");
    }

    void testAtomicCacheWrite() {
        const filesystem::path testPath =
            filesystem::temp_directory_path() / "kufar-notifier-cache-test.json";
        filesystem::remove(testPath);
        filesystem::remove(testPath.string() + ".tmp");

        saveFile(testPath.string(), "{\"version\":1}");
        require(getTextFromFile(testPath.string()) == "{\"version\":1}", "first cache write failed");
        saveFile(testPath.string(), "{\"version\":2}");
        require(getTextFromFile(testPath.string()) == "{\"version\":2}", "cache replacement failed");
        require(!filesystem::exists(testPath.string() + ".tmp"), "temporary cache file was not removed");

        const auto assertPreserved = [&](const string &body, uint64_t reserve) {
            bool rejected = false;
            try { saveFile(testPath.string(), body, reserve); }
            catch (const StorageError &) { rejected = true; }
            require(rejected, "unsafe write must be rejected");
            require(getTextFromFile(testPath.string()) == "{\"version\":2}", "failed write must preserve previous state");
            require(!filesystem::exists(testPath.string() + ".tmp"), "rejected write must not create temporary files");
        };
        validateStateSize(32ULL * 1024 * 1024 + 1);
        validateStateSize(MAX_STATE_BYTES);
        bool oversizedRejected = false;
        try { validateStateSize(MAX_STATE_BYTES + 1); }
        catch (const StorageError &) { oversizedRejected = true; }
        require(oversizedRejected, "state larger than 500 MiB must be rejected");
        assertPreserved("replacement", filesystem::space(testPath.parent_path()).capacity + 1);

        // A rename failure must also clean its one temporary file.
        const auto directoryTarget = testPath.string() + ".directory";
        filesystem::create_directory(directoryTarget);
        bool renameRejected = false;
        try { saveFile(directoryTarget, "{}", 0); }
        catch (const StorageError &) { renameRejected = true; }
        require(renameRejected && filesystem::is_directory(directoryTarget), "rename failure must preserve target");
        require(!filesystem::exists(directoryTarget + ".tmp"), "rename failure must clean temporary file");
        filesystem::remove(directoryTarget);

        filesystem::remove(testPath);
    }

    void testWordFormGrouping() {
        vector<QuerySubscription> subscriptions = {
            makeSubscription(123, {{"tag", u8"Книги"}, {"category", int(Category::electronics)}}),
            makeSubscription(123, {{"tag", u8"Книга"}}),
            makeSubscription(123, {{"tag", u8"книжека"}}),
            makeSubscription(123, {{"tag", u8"Книжка"}}),
            makeSubscription(123, {{"tag", u8"Книжный шкаф"}})
        };
        const auto saved = subscriptions;
        const auto groups = groupQueries(subscriptions);
        require(groups.size() == 2 && groups[0].searchCount == 4, "book variants form one display group");
        require(groups[0].variants.size() == 4 && groups[0].categories.size() == 2,
                "group preserves all word variants and categories");
        require(formatQueryList(subscriptions).find(u8"Мои запросы: 2") != string::npos,
                "display count reports groups");
        require(deleteKeyboard(groups)[0].size() == 2, "delete menu has one button per group");
        for (size_t i = 0; i < subscriptions.size(); ++i) {
            require(subscriptions[i].cacheKey == saved[i].cacheKey && subscriptions[i].sourceQuery == saved[i].sourceQuery,
                    "grouping must not modify searches or cache identities");
        }
        subscriptions.push_back(makeSubscription(456, {{"tag", u8"Книга"}}));
        require(removeGroupedQueries(subscriptions, 123, queryGroupKey(u8"Книга")) == 4,
                "group deletion removes all variants for the selected chat");
        require(subscriptions.size() == 2 && subscriptions.back().chatID == 456,
                "group deletion preserves other products and other recipients");
    }

    void testExecutableDirectoryResolution() {
#if defined(__linux__)
        const optional<string> executableDirectory = getWorkingDirectory();
        require(
            executableDirectory.has_value() && !executableDirectory->empty(),
            "Linux executable directory must be resolved safely"
        );
#endif
    }

    void testCycleTiming() {
        require(remainingCycleDelay(300, 180) == 120, "cycle must wait until five minutes");
        require(remainingCycleDelay(300, 320) == 0, "slow cycles must not use a negative delay");
    }

    void testCategoryMenuPages() {
        const vector<const CategoryChoice *> popular =
            categoryChoicesForPage(CategoryMenuPage::popular);
        const vector<const CategoryChoice *> more =
            categoryChoicesForPage(CategoryMenuPage::more);

        require(!popular.empty(), "popular category page must not be empty");
        require(!more.empty(), "secondary category page must not be empty");
        require(popular.size() <= 8, "popular page must stay simple");
        require(
            any_of(more.begin(), more.end(), [](const CategoryChoice *choice) {
                return choice->category == Category::computerEquipment;
            }),
            "secondary page must include computers"
        );
        require(
            any_of(more.begin(), more.end(), [](const CategoryChoice *choice) {
                return choice->category == Category::garden;
            }),
            "secondary page must include garden"
        );

        MenuState menuState;
        menuState.pendingCategories = {*more.front()};
        menuState.categoryPage = CategoryMenuPage::popular;
        require(
            selectedCategoriesText(menuState).find(more.front()->name) != string::npos,
            "selected categories must persist while switching pages"
        );
    }

    void testPriceDropRules() {
        require(priceDropPercent(10000, 7500) == 25, "discount percentage must be rounded correctly");
        require(isNewLowestPrice(7500, 10000), "a real new minimum must trigger");
        require(!isNewLowestPrice(11000, 10000), "a price rebound must not trigger");
        require(!isNewLowestPrice(10500, 10000), "a fake discount above the previous minimum must not trigger");
        require(!isNewLowestPrice(0, 10000), "zero or negotiable price must not trigger");
    }

    void testVisualMenuAndMediaDelivery() {
        const vector<vector<string>> menu = mainMenuKeyboard();
        require(menu.size() == 3, "main menu must stay compact");
        const auto ownerMenu = mainMenuKeyboard(true);
        require(ownerMenu.size() == 4 && ownerMenu.back()[0] == u8"👥 Пользователи",
                "owner alone sees user management button");
        require(menu[0].size() == 1, "primary search action must have its own row");
        require(menu[0][0] == u8"🔎 Мои запросы", "main menu must use the query list label");
        require(menu[1][0] == u8"➕ Новый запрос", "main menu must use the new query label");

        Kufar::Ad advert;
        advert.tag = u8"на запчасти";
        advert.title = u8"Магнитофон";
        advert.date = 0;
        advert.price = 2500;
        advert.sellerName = u8"Продавец";
        advert.phoneNumberIsVisible = true;
        advert.phoneNumber = "+375 29 123-45-67";
        advert.link = "https://example.test/ad";

        const string advertCard = formatAdvertCard(advert);
        require(
            advertCard.rfind(u8"#на запчасти\n", 0) == 0,
            "advert card must start with the original hashtag format"
        );
        require(
            advertCard.find(u8"Новое объявление") == string::npos,
            "advert card must not include the redundant new advert heading"
        );
        require(
            advertCard.find(u8"Запрос:") == string::npos,
            "advert card must not include the verbose query label"
        );
        require(
            advertCard.find(u8"📞 <code>+375 29 123-45-67</code>") != string::npos,
            "advert card must show the available phone number"
        );
        require(
            advertCard.find(u8"🔗 https://example.test/ad") != string::npos,
            "advert card must show the full plain Kufar link"
        );
        require(
            advertCard.find("<a href=") == string::npos,
            "advert card must not hide the Kufar URL behind an HTML link"
        );

        advert.price = 0;
        const string negotiableAdvertCard = formatAdvertCard(advert);
        require(
            negotiableAdvertCard.find(u8"Договорная") != string::npos,
            "zero Kufar price must be displayed as negotiable"
        );
        require(
            negotiableAdvertCard.find(u8"0 BYN") == string::npos,
            "negotiable price must not be displayed as zero"
        );

        require(
            advertMediaModeForImageCount(0) == AdvertMediaMode::text,
            "ad without images must be sent as text"
        );
        require(
            advertMediaModeForImageCount(1) == AdvertMediaMode::photo,
            "single-image ad must use sendPhoto"
        );
        require(
            advertMediaModeForImageCount(2) == AdvertMediaMode::album,
            "multiple images must use a media album"
        );
    }
}

int main() {
    require(commandArgument("/adduser 123", "/adduser") == optional<string>("123"), "add command argument");
    require(commandArgument("/removeuser@mybot\t456", "/removeuser") == optional<string>("456"), "scoped remove command argument");
    require(commandArgument("/adduser", "/adduser") == optional<string>(""), "empty argument opens prompt");
    require(!commandArgument("/addusers 123", "/adduser"), "different command rejected");
    require(!isTelegramCommand("/users@mybot text", "/users"), "command text cannot smuggle trailing arguments");
    {
        ProgramConfiguration configuration;
        configuration.telegramConfiguration = {"secret-must-not-leak", 123};
        configuration.kufarBearerToken = "other-secret-must-not-leak";
        configuration.access.owner = 123;
        configuration.access.initial = {123, 456, 789};
        configuration.access.change(123, 789, false);
        configuration.subscriptions = {
            makeSubscription(123, {{"tag", u8"СССР"}}),
            makeSubscription(456, {{"tag", u8"гиря"}, {"category", 4000}})
        };
        const auto backup = makeBackupConfiguration(configuration);
        require(backup.at("telegram").at("bot-token") == "", "backup must exclude bot token");
        require(backup.dump().find("must-not-leak") == string::npos, "backup must exclude all credentials");
        require(backup.at("recipients").size() == 2, "backup excludes disabled users");
        require(backup.at("recipients")[1].at("queries")[0].at("category") == 4000,
                "backup retains query filters");
        require(backup.at("recipients")[0].at("queries")[0].at("tag") == u8"СССР",
                "backup retains owner searches");
    }
    testMultiwordMatching();
    testClearAllQueries();
    testGroupedQueriesAndDeletionKeyboard();
    testWordFormGrouping();
    testAtomicCacheWrite();
    testExecutableDirectoryResolution();
    testCycleTiming();
    testCategoryMenuPages();
    testPriceDropRules();
    testVisualMenuAndMediaDelivery();
    return 0;
}
