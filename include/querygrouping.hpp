#pragma once

#include <algorithm>
#include <codecvt>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace QueryGrouping {
// Display grouping only. Never use this key for API requests, filters or cache IDs.
inline std::string wordKey(const std::string &normalizedWord) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::wstring word = converter.from_bytes(normalizedWord);
    if (word.size() < 4 || !std::all_of(word.begin(), word.end(), [](wchar_t c) {
        return c >= L'а' && c <= L'я';
    })) return normalizedWord;

    const std::wstring vowels = L"аеиоуыэюя";
    const auto firstVowel = word.find_first_of(vowels);
    if (firstVowel == std::wstring::npos) return normalizedWord;
    const size_t rv = firstVowel + 1;
    // Noun and adjective inflections from the Russian Snowball suffix classes.
    // https://snowballstem.org/algorithms/russian/stemmer.html
    // Keep at least three stem letters; do not reduce verbs or remove negation.
    static const std::vector<std::wstring> endings = {
        L"иями", L"ыми", L"ими", L"ого", L"его", L"ому", L"ему", L"ией", L"иям", L"ием", L"иях",
        L"ями", L"ами", L"ие", L"ье", L"еи", L"ии", L"ей", L"ой", L"ий", L"ям", L"ем", L"ам",
        L"ом", L"ах", L"ях", L"ию", L"ью", L"ия", L"ья", L"ые", L"ое", L"ее", L"ый", L"им",
        L"ым", L"их", L"ых", L"ую", L"юю", L"ая", L"яя", L"ою", L"ею",
        L"а", L"е", L"и", L"й", L"о", L"у", L"ы", L"ь", L"ю", L"я"
    };
    size_t longest = 0;
    for (const auto &ending : endings) {
        if (ending.size() >= word.size()) continue;
        const size_t start = word.size() - ending.size();
        if (start >= std::max<size_t>(3, rv) && ending.size() > longest &&
            word.compare(start, ending.size(), ending) == 0) longest = ending.size();
    }
    if (longest) word.resize(word.size() - longest);

    // Common book diminutives, including the user's spelling "книжека".
    // Exact stems avoid merging "книжная полка" or "книжный шкаф" into "книга".
    static const std::set<std::wstring> books = {
        L"книг", L"книжк", L"книжек", L"книжечк", L"книжечек", L"книжонк"
    };
    if (books.count(word)) word = L"книг";
    return converter.to_bytes(word);
}

inline std::string key(const std::string &normalizedQuery) {
    std::istringstream input(normalizedQuery);
    std::ostringstream output;
    std::string word;
    bool first = true;
    while (input >> word) {
        if (!first) output << ' ';
        output << wordKey(word);
        first = false;
    }
    return output.str();
}
} // namespace QueryGrouping
