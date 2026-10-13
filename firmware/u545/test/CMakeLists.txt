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

enable_testing()

# Helper: one test executable per module
function(vital_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${APP_DIR}/Inc)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(${name} PRIVATE unity m)
    if(VITAL_COVERAGE)
        target_compile_options(${name} PRIVATE --coverage -O0)
        target_link_options(${name} PRIVATE --coverage)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

vital_add_test(test_ppg
    test_ppg.c
    ${APP_DIR}/Src/ppg.c
    ${APP_DIR}/Src/ppg_model.c
)

vital_add_test(test_alarm
    test_alarm.c
    ${APP_DIR}/Src/alarm.c
    ${APP_DIR}/Src/ppg.c
    ${APP_DIR}/Src/ppg_model.c
)
