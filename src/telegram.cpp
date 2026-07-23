//
//  telegram.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <ctime>
#include "kufar.hpp"
#include "telegram.hpp"
#include "networking.hpp"
#include "json.hpp"

namespace Telegram {
    using namespace std;
    using namespace Networking;
    using nlohmann::json;

    const size_t MAX_IMAGES_IN_GROUP = 10;

    namespace {
        json parseTelegramResponse(const string &responseBody) {
            const json response = json::parse(responseBody);
            if (!response.value("ok", false)) {
                throw runtime_error(
                    "Telegram API rejected request: " +
                    response.value("description", string("unknown error"))
                );
            }
            return response;
        }

        string formatPrice(const int price) {
            if (price == 0) {
                return u8"Договорная";
            }

            ostringstream stream;
            stream << price / 100;

            const int cents = price % 100;
            if (cents != 0) {
                stream << '.' << setw(2) << setfill('0') << cents;
            }

            return stream.str() + " BYN";
        }

        string escapeHTML(const string &value) {
            string escaped;
            escaped.reserve(value.size());
            for (const char character : value) {
                switch (character) {
                    case '&': escaped += "&amp;"; break;
                    case '<': escaped += "&lt;"; break;
                    case '>': escaped += "&gt;"; break;
                    case '"': escaped += "&quot;"; break;
                    default: escaped += character; break;
                }
            }
            return escaped;
        }

        string formatDate(const time_t timestamp) {
            const tm *date = gmtime(&timestamp);
            if (date == nullptr) {
                return u8"время не указано";
            }

            ostringstream stream;
            stream << put_time(date, "%d.%m.%Y · %H:%M");
            return stream.str();
        }

        string makeAdvertCard(const Kufar::Ad &ad) {
            ostringstream text;
            if (ad.tag.has_value()) {
                text << "#" << escapeHTML(ad.tag.value()) << "\n";
            }
            text << "<b>" << escapeHTML(ad.title) << "</b>\n"
                 << u8"💰 <b>" << formatPrice(ad.price) << "</b>\n"
                 << u8"👤 " << escapeHTML(ad.sellerName) << "\n"
                 << u8"🗓 " << formatDate(ad.date) << "\n"
                 << (ad.phoneNumberIsVisible ? u8"📞 Телефон открыт" : u8"🔒 Телефон скрыт")
                 << "\n\n"
                 << u8"🔗 <a href=\"" << escapeHTML(ad.link) << "\">"
                 << escapeHTML(ad.link) << "</a>";
            return text.str();
        }

        void sendTextMessageRequest(
            const TelegramConfiguration &telegramConfiguration,
            const string &text,
            const optional<string> &replyMarkup,
            const optional<string> &parseMode
        ) {
            const string url = "https://api.telegram.org/bot" + telegramConfiguration.botToken +
                               "/sendMessage";
            json request = {
                {"chat_id", telegramConfiguration.chatID},
                {"text", text},
                {"disable_web_page_preview", true}
            };

            if (replyMarkup.has_value()) {
                request["reply_markup"] = json::parse(replyMarkup.value());
            }
            if (parseMode.has_value()) {
                request["parse_mode"] = parseMode.value();
            }

            parseTelegramResponse(postJSONToURL(url, request.dump()));
        }
    }

    string formatAdvertCard(const Kufar::Ad &ad) {
        return makeAdvertCard(ad);
    }

    optional<int64_t> getLatestChatID(const string &botToken) {
        const string url = "https://api.telegram.org/bot" + botToken + "/getUpdates";
        const json response = parseTelegramResponse(getJSONFromURL(url));

        if (!response.contains("result")) {
            return nullopt;
        }

        const json &updates = response.at("result");
        for (auto update = updates.rbegin(); update != updates.rend(); ++update) {
            for (const char *messageType : {"message", "edited_message", "channel_post"}) {
                if (update->contains(messageType) && update->at(messageType).contains("chat")) {
                    return update->at(messageType).at("chat").at("id").get<int64_t>();
                }
            }
        }

        return nullopt;
    }

    vector<TelegramUpdate> getUpdates(const string &botToken, const int64_t offset) {
        const string url = "https://api.telegram.org/bot" + botToken +
                           "/getUpdates?timeout=0&offset=" + to_string(offset);
        const json response = parseTelegramResponse(getJSONFromURL(url));

        if (!response.contains("result")) {
            throw runtime_error("Telegram getUpdates returned an invalid response");
        }

        vector<TelegramUpdate> result;
        for (const json &update : response.at("result")) {
            if (!update.contains("update_id")) {
                continue;
            }

            TelegramUpdate parsedUpdate = {
                update.at("update_id").get<int64_t>(),
                0,
                ""
            };

            if (update.contains("message")) {
                const json &message = update.at("message");
                if (message.contains("chat") && message.contains("text")) {
                    parsedUpdate.chatID = message.at("chat").at("id").get<int64_t>();
                    parsedUpdate.text = message.at("text").get<string>();
                }
            }

            result.push_back(parsedUpdate);
        }

        return result;
    }

    void sendTextMessage(const TelegramConfiguration &telegramConfiguration, const string &text) {
        sendTextMessageRequest(telegramConfiguration, text, nullopt, nullopt);
    }

    void sendTextMessageWithKeyboard(
        const TelegramConfiguration &telegramConfiguration,
        const string &text,
        const vector<vector<string>> &buttons
    ) {
        json keyboard = json::array();
        for (const vector<string> &row : buttons) {
            json keyboardRow = json::array();
            for (const string &buttonText : row) {
                keyboardRow.push_back({{"text", buttonText}});
            }
            keyboard.push_back(keyboardRow);
        }

        const json replyMarkup = {
            {"keyboard", keyboard},
            {"resize_keyboard", true},
            {"one_time_keyboard", false},
            {"input_field_placeholder", u8"Выберите действие"}
        };
        sendTextMessageRequest(telegramConfiguration, text, replyMarkup.dump(), nullopt);
    }

    void setBotCommands(const string &botToken) {
        const json commands = json::array({
            {{"command", "menu"}, {"description", u8"Открыть главное меню"}},
            {{"command", "queries"}, {"description", u8"Показать мои запросы"}},
            {{"command", "add"}, {"description", u8"Создать новый запрос"}},
            {{"command", "delete"}, {"description", u8"Удалить запрос"}},
            {{"command", "status"}, {"description", u8"Проверить состояние бота"}},
            {{"command", "help"}, {"description", u8"Как всё работает"}}
        });
        const string url = "https://api.telegram.org/bot" + botToken + "/setMyCommands";
        const json request = {{"commands", commands}};
        parseTelegramResponse(postJSONToURL(url, request.dump()));
    }

    string makeImageGroupJSON(const vector<string> &images, const string &caption) {
        json j_array = json::array();
        for (size_t i = 0; (i < images.size()) && (i < MAX_IMAGES_IN_GROUP); ++i) {
            json j_list = json::object({
                    {"type", "photo"},
                    {"media", images[i]}
            });
            
            if (i == 0) {
                j_list.push_back({"caption", caption});
                j_list.push_back({"parse_mode", "HTML"});
            }
            j_array.push_back(j_list);
        }
        
        return j_array.dump();
    }

    AdvertMediaMode advertMediaModeForImageCount(const size_t imageCount) {
        if (imageCount == 0) {
            return AdvertMediaMode::text;
        }
        if (imageCount == 1) {
            return AdvertMediaMode::photo;
        }
        return AdvertMediaMode::album;
    }

    void sendAdvert(const TelegramConfiguration &telegramConfiguration, const Kufar::Ad &ad) {
        const string text = formatAdvertCard(ad);
        const AdvertMediaMode mediaMode = advertMediaModeForImageCount(ad.images.size());

        if (mediaMode == AdvertMediaMode::text) {
            sendTextMessageRequest(telegramConfiguration, text, nullopt, string("HTML"));
            return;
        }

        if (mediaMode == AdvertMediaMode::photo) {
            const string url = "https://api.telegram.org/bot" + telegramConfiguration.botToken +
                               "/sendPhoto";
            const json request = {
                {"chat_id", telegramConfiguration.chatID},
                {"photo", ad.images.front()},
                {"caption", text},
                {"parse_mode", "HTML"}
            };
            parseTelegramResponse(postJSONToURL(url, request.dump()));
            return;
        }

        const string url = "https://api.telegram.org/bot" + telegramConfiguration.botToken +
                           "/sendMediaGroup";
        const json request = {
            {"chat_id", telegramConfiguration.chatID},
            {"media", json::parse(makeImageGroupJSON(ad.images, text))}
        };
        parseTelegramResponse(postJSONToURL(url, request.dump()));
    }

    void sendPriceDrop(
        const TelegramConfiguration &telegramConfiguration,
        const Kufar::Ad &ad,
        const int previousPrice
    ) {
        const int difference = previousPrice - ad.price;
        const int percentage = previousPrice > 0
            ? static_cast<int>(
                (static_cast<long long>(difference) * 100 + previousPrice / 2) / previousPrice
            )
            : 0;
        const string percentageText = percentage > 0
            ? u8"−" + to_string(percentage) + "%"
            : u8"снижение цены";
        string text = u8"💸 <b>Новая минимальная цена</b>\n\n"
                      "<b>" + escapeHTML(ad.title) + "</b>\n"
                      u8"💰 <s>" + formatPrice(previousPrice) + "</s> → <b>" +
                      formatPrice(ad.price) + "</b>\n"
                      u8"📉 " + percentageText + u8" · выгода " + formatPrice(difference) + "\n\n"
                      u8"🔗 <a href=\"" + escapeHTML(ad.link) + "\">" +
                      escapeHTML(ad.link) + "</a>";

        sendTextMessageRequest(telegramConfiguration, text, nullopt, string("HTML"));
    }
};
