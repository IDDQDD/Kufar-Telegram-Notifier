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
}

int main() {
    testMultiwordMatching();
    testGroupedQueriesAndDeletionKeyboard();
    testAtomicCacheWrite();
    testCycleTiming();
    testCategoryMenuPages();
    testPriceDropRules();
    return 0;
}
