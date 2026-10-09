#include "statelock.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

int main(int argc, char **argv) try {
#if defined(__linux__) || defined(__APPLE__)
    if (argc == 3 && std::string(argv[1]) == "--try-lock") {
        try { StateLock lock(argv[2]); return 0; }
        catch (const std::exception &) { return 3; }
    }
    const auto cache = std::filesystem::temp_directory_path() /
        ("kufar-lock-test-" + std::to_string(getpid()) + ".json");
    { std::ofstream file(cache); file << "[]"; }
    const auto childResult = [&]() {
        const pid_t child = fork();
        if (child < 0) throw std::runtime_error("Unable to fork lock test");
        if (child == 0) {
            execl(argv[0], argv[0], "--try-lock", cache.c_str(), static_cast<char *>(nullptr));
            _exit(4);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
            throw std::runtime_error("Lock test child did not exit normally");
        return WEXITSTATUS(status);
    };
    {
        StateLock owner(cache.string());
        if (childResult() != 3) throw std::runtime_error("Second bot process must not acquire the same cache");
    }
    if (childResult() != 0) throw std::runtime_error("Lock must be released on exit without deleting its file");
    std::string body;
    { std::ifstream file(cache); file >> body; }
    if (body != "[]") throw std::runtime_error("Locking must not change cached state");
    std::filesystem::remove(cache);
    std::filesystem::remove(cache.string() + ".lock");
    std::cout << "Cross-process state lock tests passed\n";
#else
    (void)argc;
    (void)argv;
    std::cout << "State lock tests require the Linux or macOS runtime\n";
#endif
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
