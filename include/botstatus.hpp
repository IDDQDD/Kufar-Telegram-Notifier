#pragma once

#include <string>
#include "networking.hpp"

namespace BotStatus {
inline std::string message(Networking::ProxyAvailability availability) {
    const std::string bot = u8"🟢 Бот работает\n";
    switch (availability) {
        case Networking::ProxyAvailability::available:
            return bot + u8"🟢 Прокси-серверы доступны";
        case Networking::ProxyAvailability::unavailable:
            return bot + u8"🔴 Прокси-серверы недоступны";
        case Networking::ProxyAvailability::notConfigured:
            return bot + u8"⚪ Прокси-серверы не настроены";
        case Networking::ProxyAvailability::unknown:
            return bot + u8"⏳ Доступность прокси-серверов ещё не проверена";
    }
    return bot + u8"⏳ Доступность прокси-серверов ещё не проверена";
}
} // namespace BotStatus
