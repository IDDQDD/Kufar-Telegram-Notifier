#pragma once
#include <optional>
#include <stdexcept>
#include <string>

namespace NetworkPolicy {
inline bool isKufarURL(const std::string &url) {
    return url.rfind("https://api.kufar.by/", 0) == 0 ||
           url.rfind("https://searchapi.kufar.by/", 0) == 0;
}
inline std::optional<std::string> kufarProxy(const std::string &url, const std::string &setting) {
    if (setting.empty() || !isKufarURL(url)) return std::nullopt;
    const bool supported = setting.rfind("http://", 0) == 0 || setting.rfind("https://", 0) == 0 ||
                           setting.rfind("socks4://", 0) == 0 || setting.rfind("socks4a://", 0) == 0 ||
                           setting.rfind("socks5://", 0) == 0 || setting.rfind("socks5h://", 0) == 0;
    if (!supported || setting.size() > 2048 || setting.find_first_of(" \t\r\n") != std::string::npos)
        throw std::runtime_error("Invalid KUFAR_PROXY: use an HTTP, HTTPS, SOCKS4 or SOCKS5 proxy URL");
    return setting;
}
}
