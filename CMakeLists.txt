cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

project(
    Kufar-Telegram-Notifier
    VERSION 2.6
)

include_directories(include)

add_executable(
    Kufar-Telegram-Notifier
    
    src/kufar.cpp
    src/helperfunctions.cpp
    src/telegram.cpp
    src/networking.cpp
    src/main.cpp
)

target_link_libraries(Kufar-Telegram-Notifier curl)
target_compile_options(Kufar-Telegram-Notifier PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)

if(BUILD_TESTING)
    add_executable(
        Kufar-Telegram-Notifier-Tests

        tests/notifier_tests.cpp
        src/kufar.cpp
        src/helperfunctions.cpp
        src/telegram.cpp
        src/networking.cpp
    )
    target_link_libraries(Kufar-Telegram-Notifier-Tests curl)
    target_compile_options(Kufar-Telegram-Notifier-Tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME notifier-tests COMMAND Kufar-Telegram-Notifier-Tests)
endif()
