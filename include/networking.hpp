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

namespace Networking {
    std::string urlEncode(const std::string &);
    std::string getJSONFromURL(const std::string &);
    std::string getJSONFromURL(const std::string &, const std::vector<std::string> &);
    std::string postJSONToURL(const std::string &, const std::string &);
    std::string postDocumentToURL(const std::string &, int64_t, const std::string &, const std::string &, const std::string &);
};

#endif /* networking_hpp */
