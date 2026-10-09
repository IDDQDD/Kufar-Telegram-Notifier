//
//  networking.hpp
//  Kufar Telegram Notifier
//
//  Created by Macintosh on 04.06.2022.
//

#ifndef networking_hpp
#define networking_hpp

#include <string>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace Networking {
    class HTTPError : public std::runtime_error {
        long code;
        std::string body;
    public:
        HTTPError(long status, const std::string &response)
            : std::runtime_error("HTTP request returned status " + std::to_string(status)),
              code(status), body(response.substr(0, 4096)) {}
        long status() const { return code; }
        const std::string &responseBody() const { return body; }
    };
    std::string urlEncode(const std::string &);
    std::string getJSONFromURL(const std::string &);
    std::string getJSONFromURL(const std::string &, const std::vector<std::string> &);
    std::string postJSONToURL(const std::string &, const std::string &);
    std::string postDocumentToURL(const std::string &, int64_t, const std::string &, const std::string &, const std::string &);
};

#endif /* networking_hpp */
