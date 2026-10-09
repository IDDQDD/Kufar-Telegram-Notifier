#pragma once

#include <stdexcept>
#include <string>
#if defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

class StateLock {
    int descriptor = -1;
public:
    explicit StateLock(const std::string &cachePath) {
#if defined(__linux__) || defined(__APPLE__)
        descriptor = open((cachePath + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor < 0) throw std::runtime_error("Unable to open cache lock: " + std::string(std::strerror(errno)));
        if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            close(descriptor);
            descriptor = -1;
            if (error == EWOULDBLOCK || error == EAGAIN)
                throw std::runtime_error("Another bot is already using this cache; stop the duplicate instance");
            throw std::runtime_error("Unable to lock cache: " + std::string(std::strerror(error)));
        }
#else
        (void)cachePath; // Native Windows checks compile the Linux bot's sources only.
#endif
    }
    StateLock(const StateLock &) = delete;
    StateLock &operator=(const StateLock &) = delete;
    ~StateLock() {
#if defined(__linux__) || defined(__APPLE__)
        if (descriptor >= 0) close(descriptor);
#endif
    }
};
