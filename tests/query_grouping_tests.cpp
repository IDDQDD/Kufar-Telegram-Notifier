#include "querygrouping.hpp"
#include <iostream>
#include <stdexcept>

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
int main() try {
    using QueryGrouping::key;
    require(key(u8"книга") == key(u8"книги"), "singular and plural books grouped");
    for (const auto *variant : {u8"книжка", u8"книжки", u8"книжечка", u8"книжечки", u8"книжека", u8"книгами"})
        require(key(variant) == key(u8"книга"), "book diminutives grouped");
    require(key(u8"гараж") == key(u8"гаражи"), "garage inflections grouped");
    require(key(u8"гаражами") == key(u8"гараж"), "longest noun ending removed");
    require(key(u8"блок питания") == key(u8"блоки питания"), "multiword inflections grouped");
    require(key(u8"новый телефон") == key(u8"новые телефоны"), "adjective and noun inflections grouped");
    require(key(u8"гиря 24 кг") == key(u8"гири 24 кг"), "weight queries retain numbers and units");
    require(key(u8"гиря 24 кг") != key(u8"гиря 32 кг"), "different numbers stay separate");
    require(key(u8"книжный шкаф") != key(u8"книга"), "different products stay separate");
    require(key(u8"не включается") != key(u8"включается"), "negation stays separate");
    require(key(u8"не проверял") != key(u8"проверял не"), "word order is preserved");
    require(key("iphone 13") != key("iphone 14"), "Latin model numbers stay separate");
    require(key(" ssd  1tb ") == key("ssd 1tb"), "extra spaces ignored");
    std::cout << "Query grouping tests passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
