#include "networking.hpp"
#include <iostream>
#include <cstdlib>
#include <curl/curl.h>
#undef curl_easy_perform
extern "C" CURLcode curl_easy_perform(CURL *);

extern "C" CURLcode networkProbePerform(CURL *curl) {
    // Trust the offline TLS fixture only in this test binary; verification stays enabled.
    const char *certificate = std::getenv("NETWORK_TEST_CA_FILE");
    if (certificate) curl_easy_setopt(curl, CURLOPT_CAINFO, certificate);
    return curl_easy_perform(curl);
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int status = 0;
    for (int index = 1; index < argc; ++index) {
        try { std::cout << Networking::getJSONFromURL(argv[index]) << '\n'; }
        catch (const Networking::ProxyError &error) { std::cerr << "ProxyError: " << error.what() << '\n'; status = 1; }
        catch (const std::exception &error) { std::cerr << error.what() << '\n'; status = 1; }
    }
    return status;
}
