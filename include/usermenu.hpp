#pragma once

#include <string>
#include <vector>
#include "useraccess.hpp"

namespace UserMenu {
inline std::string label(const Lifecycle::Access &access, int64_t id) {
    const auto name = access.names.find(id);
    return std::to_string(id) + (name == access.names.end() ? "" : " · " + name->second);
}

inline std::vector<std::string> pages(const Lifecycle::Access &access) {
    const std::string heading = u8"👥 Пользователи\n\n";
    const std::string footer = u8"\nНовый пользователь пишет боту /start и передаёт вам свой ID. При добавлении можно указать имя: 123456789 Иван Петров.";
    std::vector<std::string> pages;
    std::string page = heading;
    for (const auto id : access.users()) {
        const std::string line = label(access, id) + (id == access.owner ? u8" · владелец" : "") + "\n";
        if (page.size() + line.size() > 3500) {
            pages.push_back(page);
            page = heading;
        }
        page += line;
    }
    if (page.size() + footer.size() > 3500) {
        pages.push_back(page);
        page = heading;
    }
    pages.push_back(page + footer);
    return pages;
}

} // namespace UserMenu
