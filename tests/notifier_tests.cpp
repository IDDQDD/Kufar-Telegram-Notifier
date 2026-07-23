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

        const size_t removedCount = removeGroupedQueries(
            subscriptions,
            chatID,
            normalizeTitle(u8"Блок питания")
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

        filesystem::remove(testPath);
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
        require(menu[0].size() == 1, "primary search action must have its own row");
        require(menu[0][0] == u8"🔎 Мои запросы", "main menu must use the query list label");
        require(menu[1][0] == u8"➕ Новый запрос", "main menu must use the new query label");

        Kufar::Ad advert;
        advert.tag = u8"на запчасти";
        advert.title = u8"Магнитофон";
        advert.date = 0;
        advert.price = 2500;
        advert.sellerName = u8"Продавец";
        advert.phoneNumberIsVisible = false;
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
            advertCard.find("https://example.test/ad</a>") != string::npos,
            "advert card must show the full Kufar link"
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
    testMultiwordMatching();
    testGroupedQueriesAndDeletionKeyboard();
    testAtomicCacheWrite();
    testExecutableDirectoryResolution();
    testCycleTiming();
    testCategoryMenuPages();
    testPriceDropRules();
    testVisualMenuAndMediaDelivery();
    return 0;
}
