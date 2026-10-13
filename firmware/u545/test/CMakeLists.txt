# Host (PC) unit tests for hardware-independent code.
# Build:  cmake -S test -B build-test -G Ninja
#         cmake --build build-test
#         ctest --test-dir build-test --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(vital_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.0
)
FetchContent_MakeAvailable(unity)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../App)

option(VITAL_COVERAGE "Build tests with gcov coverage" OFF)

add_executable(test_ppg
    test_ppg.c
    ${APP_DIR}/Src/ppg.c
    ${APP_DIR}/Src/ppg_model.c
)
target_include_directories(test_ppg PRIVATE ${APP_DIR}/Inc)
target_compile_options(test_ppg PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_ppg PRIVATE unity m)

if(VITAL_COVERAGE)
    target_compile_options(test_ppg PRIVATE --coverage -O0)
    target_link_options(test_ppg PRIVATE --coverage)
endif()

enable_testing()
add_test(NAME test_ppg COMMAND test_ppg)
