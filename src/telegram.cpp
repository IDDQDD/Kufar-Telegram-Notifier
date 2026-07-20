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
#include "kufar.hpp"
#include "telegram.hpp"
#include "networking.hpp"
#include "json.hpp"

namespace Telegram {
    using namespace std;
    using namespace Networking;
    using nlohmann::json;

    const short int MAX_IMAGES_IN_GROUP = 10;

    namespace {
        string formatPrice(const int price) {
            ostringstream stream;
            stream << price / 100;

            const int cents = price % 100;
            if (cents != 0) {
                stream << '.' << setw(2) << setfill('0') << cents;
            }

            return stream.str() + " BYN";
        }

        void sendTextMessageRequest(
            const TelegramConfiguration &telegramConfiguration,
            const string &text,
            const optional<string> &replyMarkup
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

            postJSONToURL(url, request.dump());
        }
    }

    optional<int64_t> getLatestChatID(const string &botToken) {
        const string url = "https://api.telegram.org/bot" + botToken + "/getUpdates";
        const json response = json::parse(getJSONFromURL(url));

        if (!response.value("ok", false) || !response.contains("result")) {
            return nullopt;
        }

        const json &updates = response.at("result");
        for (auto update = updates.rbegin(); update != updates.rend(); ++update) {
            for (const string &messageType : {"message", "edited_message", "channel_post"}) {
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
        const json response = json::parse(getJSONFromURL(url));

        if (!response.value("ok", false) || !response.contains("result")) {
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
        sendTextMessageRequest(telegramConfiguration, text, nullopt);
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
        sendTextMessageRequest(telegramConfiguration, text, replyMarkup.dump());
    }

    void setBotCommands(const string &botToken) {
        const json commands = json::array({
            {{"command", "menu"}, {"description", u8"Открыть простое меню"}},
            {{"command", "queries"}, {"description", u8"Показать мои запросы"}},
            {{"command", "add"}, {"description", u8"Добавить запрос"}},
            {{"command", "delete"}, {"description", u8"Удалить запрос"}},
            {{"command", "status"}, {"description", u8"Проверить работу бота"}}
        });
        const string url = "https://api.telegram.org/bot" + botToken + "/setMyCommands";
        const json request = {{"commands", commands}};
        postJSONToURL(url, request.dump());
    }

    string makeImageGroupJSON(const vector<string> &images, const string &caption) {
        json j_array = json::array();
        for (int i = 0; (i < images.size()) && (i < MAX_IMAGES_IN_GROUP); i++){
            json j_list = json::object({
                    {"type", "photo"},
                    {"media", images[i]}
            });
            
            if (i == 0) {
                j_list.push_back({"caption", caption});
            }
            j_array.push_back(j_list);
        }
        
        return j_array.dump();
    }

    void sendAdvert(const TelegramConfiguration &telegramConfiguration, const Kufar::Ad &ad) {
        string formattedTime = ctime(&ad.date);
        formattedTime.pop_back();
        
        string text = "";
        
        if (ad.tag.has_value()) {
            text += "#" + ad.tag.value() + "\n";
        }
        
        text += "Название: " + ad.title + "\n"
                "Дата: " + formattedTime + "\n"
                "Цена: " + formatPrice(ad.price) + "\n\n"
                "Имя продавца: " + ad.sellerName + "\n"
                "Номер телефона не скрыт: " + (ad.phoneNumberIsVisible ? "Да" : "Нет") + "\n"
                "Ссылка: " + ad.link;
        
        string url = "https://api.telegram.org/bot" + telegramConfiguration.botToken;
        if (!ad.images.empty()) {
            url += "/sendMediaGroup?chat_id=" + to_string(telegramConfiguration.chatID) + "&"
                   "media=" + urlEncode(makeImageGroupJSON(ad.images, text));
        } else {
            url += "/sendPhoto?chat_id=" + to_string(telegramConfiguration.chatID) + "&"
                   "caption=" + urlEncode(text) + "&"
                   "photo=https://via.placeholder.com/1080";
            /*url += "/sendMessage?chat_id=" + telegramConfiguration.chatID_or_Username + "&"
                   "text=" + urlEncode("[No Photo]\n\n" + text);*/
        }
        
        getJSONFromURL(url);
    }

    void sendPriceDrop(
        const TelegramConfiguration &telegramConfiguration,
        const Kufar::Ad &ad,
        const int previousPrice
    ) {
        const int difference = previousPrice - ad.price;
        string text = "💸 Цена снижена\n\n"
                      "Название: " + ad.title + "\n"
                      "Было: " + formatPrice(previousPrice) + "\n"
                      "Стало: " + formatPrice(ad.price) + "\n"
                      "Снижение: " + formatPrice(difference) + "\n\n"
                      "Ссылка: " + ad.link;

        sendTextMessage(telegramConfiguration, text);
    }
};
