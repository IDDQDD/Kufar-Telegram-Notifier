#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include "json.hpp"

namespace Lifecycle {
struct ProxyHealth {
    static constexpr int alertThreshold = 3;
    std::string key;
    int failures = 0;
    bool notified = false;

    void configure(const std::string &proxy) {
        // This identifies a configuration for restart recovery; credentials are never stored.
        key = proxy.empty() ? "" : std::to_string(std::hash<std::string>{}(proxy));
        reset();
    }
    void failed() { if (!key.empty()) failures = std::min(alertThreshold, failures + 1); }
    void reset() { failures = 0; notified = false; }
    bool pending() const { return !key.empty() && failures == alertThreshold && !notified; }
    nlohmann::json save() const {
        if (key.empty()) return nlohmann::json::object();
        return {{"proxy-key", key}, {"failures", failures}, {"notified", notified}};
    }
    void load(const nlohmann::json &data) {
        reset();
        if (key.empty() || !data.is_object() || data.value("proxy-key", std::string{}) != key) return;
        failures = std::clamp(data.value("failures", 0), 0, alertThreshold);
        notified = failures == alertThreshold && data.value("notified", false);
    }
};

} // namespace Lifecycle
