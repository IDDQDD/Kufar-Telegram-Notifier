//
//  networking.cpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#include <iostream>
#include <stdexcept>
#include <curl/curl.h>
#include "networking.hpp"
#include "helperfunctions.hpp"

namespace Networking {
    using std::string;

    namespace {
        static size_t writeFunction(void *ptr, size_t size, size_t nmemb, string *data) {
            data->append((char*)ptr, size * nmemb);
            return size * nmemb;
        }

        void ensureCurlInitialized() {
            static const bool initialized = []() {
                if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                    throw std::runtime_error("Unable to initialize HTTP client");
                }
                return true;
            }();
            (void)initialized;
        }

        void validateRequestResult(const CURLcode result, const long httpStatus) {
            if (result != CURLE_OK) {
                throw std::runtime_error(
                    "HTTP request failed: " + string(curl_easy_strerror(result))
                );
            }
            if (httpStatus < 200 || httpStatus >= 300) {
                throw std::runtime_error(
                    "HTTP request returned status " + std::to_string(httpStatus)
                );
            }
        }
    }

    string urlEncode(const string &text) {
        ensureCurlInitialized();
        CURL *curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Unable to create HTTP client");
        }
        char *encoded = curl_easy_escape(curl, text.c_str(), 0);
        if (!encoded) {
            curl_easy_cleanup(curl);
            throw std::runtime_error("Unable to encode URL value");
        }
        string tempVariable = encoded;
        curl_free(encoded);
        curl_easy_cleanup(curl);
        return tempVariable;
    }

    string getJSONFromURL(const string &url) {
        // URLs may contain the Telegram bot token, so never print them.
        DEBUG_MSG("[HTTP GET]");

        ensureCurlInitialized();
        auto curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Unable to create HTTP client");
        }
        string responseString;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);

        const CURLcode result = curl_easy_perform(curl);
        long httpStatus = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        curl_easy_cleanup(curl);
        validateRequestResult(result, httpStatus);
        return responseString;
    }

    string postJSONToURL(const string &url, const string &body) {
        ensureCurlInitialized();
        auto curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Unable to create HTTP client");
        }
        string responseString;

        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);

        const CURLcode result = curl_easy_perform(curl);
        long httpStatus = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        validateRequestResult(result, httpStatus);

        return responseString;
    }

}
