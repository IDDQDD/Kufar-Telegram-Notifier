#include "networkpolicy.hpp"
#include <iostream>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        for (const char *scheme : {"http", "https", "socks4", "socks4a", "socks5", "socks5h"}) {
            const std::string proxy = std::string(scheme) + "://127.0.0.1:1080";
            for (const char *url : {"https://api.kufar.by/search-api/v2/search/rendered-paginated",
                                    "https://searchapi.kufar.by/v1/search/rendered-paginated",
                                    "https://api.kufar.by/search-api/v2/item/1/phone"}) {
                require(NetworkPolicy::kufarProxy(url, proxy) == proxy, "supported proxy is preserved");
            }
            for (const char *url : {"https://api.telegram.org/botoffline/getUpdates",
                                    "https://api.kufar.by.evil.invalid/search",
                                    "https://api.kufar.by@evil.invalid/search",
                                    "https://evil.invalid/api.kufar.by/search"}) {
                require(!NetworkPolicy::kufarProxy(url, proxy), "unrelated requests never use the proxy");
            }
        }
        require(!NetworkPolicy::kufarProxy("https://api.kufar.by/search", ""), "empty setting keeps direct routing");
        for (const std::string setting : {std::string("socks4x://127.0.0.1:1080"),
                                          std::string("socks4://127.0.0.1:1080\nprivate-test-secret"),
                                          std::string("socks4a://127.0.0.1:1080 "),
                                          std::string("socks4://") + std::string(2048, 'x')}) {
            bool rejected = false;
            try { NetworkPolicy::kufarProxy("https://api.kufar.by/search", setting); }
            catch (const std::runtime_error &error) {
                rejected = true;
                require(std::string(error.what()).find("private-test-secret") == std::string::npos,
                        "invalid setting is never echoed in errors");
            }
            require(rejected, "malformed proxy is rejected");
        }
        std::cout << "Network policy tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
