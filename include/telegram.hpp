//
//  telegram.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef Header_hpp
#define Header_hpp

#include <cstdint>
#include <string>

namespace Telegram {
    struct TelegramConfiguration {
        std::string botToken;
        int64_t chatID;
    };

    void sendAdvert(const TelegramConfiguration &, const Kufar::Ad &);
};
#endif /* Header_hpp */
