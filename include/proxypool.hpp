#pragma once

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "networkpolicy.hpp"

namespace NetworkPolicy {
inline std::vector<std::string> configuredKufarProxies() {
    std::vector<std::string> proxies;
    const auto append = [&](const std::string &value) {
        if (value.empty()) return;
        kufarProxy("https://api.kufar.by/", value);
        if (std::find(proxies.begin(), proxies.end(), value) == proxies.end()) proxies.push_back(value);
        if (proxies.size() > 32) throw std::runtime_error("Kufar proxy pool exceeds 32 proxies");
    };
    const char *primary = std::getenv("KUFAR_PROXY");
    if (primary) append(primary);
    const char *pool = std::getenv("KUFAR_PROXY_POOL");
    const char *poolFile = std::getenv("KUFAR_PROXY_POOL_FILE");
    if (pool) {
        std::string values = pool;
        std::replace(values.begin(), values.end(), ';', '\n');
        std::istringstream input(values);
        std::string value;
        while (input >> value) append(value);
    }
    // An explicit pool (including an empty one) overrides the bundled reserves.
    const std::string path = poolFile ? poolFile : (!pool && !proxies.empty() ? "kufar-proxies.txt" : "");
    if (!path.empty()) {
        std::ifstream input(path);
        if (!input && poolFile) throw std::runtime_error("Unable to read KUFAR_PROXY_POOL_FILE");
        std::string line;
        while (std::getline(input, line)) {
            const auto start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos || line[start] == '#') continue;
            const auto end = line.find_last_not_of(" \t\r");
            append(line.substr(start, end - start + 1));
        }
    }
    return proxies;
}

class ProxyPool {
private:
    std::vector<std::string> proxies;
    size_t active = 0;
public:
    explicit ProxyPool(std::vector<std::string> values)
        : proxies(std::move(values)) {}
    bool empty() const { return proxies.empty(); }
    const std::string &at(size_t index) const { return proxies.at(index); }
    std::optional<size_t> next(const std::vector<size_t> &attempted) const {
        for (size_t offset = 0; offset < proxies.size(); ++offset) {
            const size_t index = (active + offset) % proxies.size();
            if (std::find(attempted.begin(), attempted.end(), index) == attempted.end()) return index;
        }
        return std::nullopt;
    }
    void connected(size_t index) { active = index; }
};
}
