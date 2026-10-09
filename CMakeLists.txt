cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

project(
    Kufar-Telegram-Notifier
    VERSION 2.9.3
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
    add_executable(StateLock-Tests tests/statelock_tests.cpp)
    add_test(NAME state-lock-tests COMMAND StateLock-Tests)
    add_executable(Query-Grouping-Tests tests/query_grouping_tests.cpp)
    add_test(NAME query-grouping-tests COMMAND Query-Grouping-Tests)
    add_executable(Kufar-Search-Tests tests/kufar_tests.cpp src/kufar.cpp src/helperfunctions.cpp)
    add_test(NAME kufar-search-tests COMMAND Kufar-Search-Tests)
    add_executable(Telegram-Tests tests/telegram_tests.cpp src/telegram.cpp)
    add_test(NAME telegram-tests COMMAND Telegram-Tests)
    add_executable(Lifecycle-Tests tests/lifecycle_tests.cpp)
    add_test(NAME lifecycle-tests COMMAND Lifecycle-Tests)
    add_executable(Menu-Tests tests/menu_tests.cpp src/kufar.cpp src/helperfunctions.cpp src/telegram.cpp)
    target_compile_options(Menu-Tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME menu-tests COMMAND Menu-Tests)
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
