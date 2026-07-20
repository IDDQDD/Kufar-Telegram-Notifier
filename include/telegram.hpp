//
//  telegram.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef Header_hpp
#define Header_hpp

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Kufar {
    struct Ad;
}

namespace Telegram {
    struct TelegramConfiguration {
        std::string botToken;
        int64_t chatID;
    };

    struct TelegramUpdate {
        int64_t updateID;
        int64_t chatID;
        std::string text;
    };

    std::optional<int64_t> getLatestChatID(const std::string &);
    std::vector<TelegramUpdate> getUpdates(const std::string &, int64_t);
    void sendTextMessage(const TelegramConfiguration &, const std::string &);
    void sendTextMessageWithKeyboard(
        const TelegramConfiguration &,
        const std::string &,
        const std::vector<std::vector<std::string>> &
    );
    void setBotCommands(const std::string &);
    void sendAdvert(const TelegramConfiguration &, const Kufar::Ad &);
    void sendPriceDrop(const TelegramConfiguration &, const Kufar::Ad &, int);
};
#endif /* Header_hpp */
