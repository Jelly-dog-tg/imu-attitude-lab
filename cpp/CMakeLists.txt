# CMake project for the IMU Attitude Lab C++17 processing core.
#
# Designed to build unchanged on:
#   * Linux with GCC or Clang   (the primary target: Ubuntu / WSL2)
#   * Windows with MinGW-w64    (verified locally)
#   * Windows with MSVC         (covered by the CI matrix)
#
# Everything is standard C++17 plus Threads — no third-party dependency is
# downloaded at configure time, so the build works behind a restrictive proxy.
cmake_minimum_required(VERSION 3.16)

project(imu_attitude_lab
    VERSION 1.0.0
    DESCRIPTION "C++17 processing core for the IMU Attitude Lab"
    LANGUAGES CXX)

# --------------------------------------------------------------------------- #
# Options
# --------------------------------------------------------------------------- #
option(IMU_LAB_BUILD_TESTS "Build the unit-test binary" ON)
option(IMU_LAB_BUILD_BENCH "Build the performance/stability harness" ON)
option(IMU_LAB_WERROR "Treat compiler warnings as errors" OFF)
set(IMU_LAB_SANITIZER "" CACHE STRING
    "Instrument the build with a sanitizer: '' | address+undefined | thread")

# --------------------------------------------------------------------------- #
# Toolchain-independent build settings
# --------------------------------------------------------------------------- #
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Put every executable in <build>/bin for every generator and configuration, so
# scripts and CI do not have to guess between build/ and build/Release/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# Export compile_commands.json for clangd / clang-tidy users.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# --------------------------------------------------------------------------- #
# Sanitizers must be applied before any target is created.
# --------------------------------------------------------------------------- #
if(IMU_LAB_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(IMU_LAB_SANITIZER STREQUAL "address+undefined")
            add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=address,undefined)
            message(STATUS "Sanitizer: address + undefined")
        elseif(IMU_LAB_SANITIZER STREQUAL "thread")
            add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=thread)
            message(STATUS "Sanitizer: thread")
        else()
            message(FATAL_ERROR "Unsupported IMU_LAB_SANITIZER='${IMU_LAB_SANITIZER}'")
        endif()
    else()
        message(WARNING "Sanitizers requested but '${CMAKE_CXX_COMPILER_ID}' is not GCC/Clang; ignoring")
    endif()
endif()

# --------------------------------------------------------------------------- #
# Warning policy
# --------------------------------------------------------------------------- #
function(imu_lab_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(IMU_LAB_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-qual)
        if(IMU_LAB_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# --------------------------------------------------------------------------- #
# Core library
# --------------------------------------------------------------------------- #
add_library(imu_lab_core STATIC
    src/csv_reader.cpp
    src/frame_source.cpp
    src/jump_detector.cpp
    src/metrics.cpp
    src/number_parser.cpp
    src/pipeline.cpp
    src/protocol_parser.cpp
    src/statistics.cpp
)

target_include_directories(imu_lab_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Keeping the public include directory as an INTERFACE property means consumers
# (tests, CLI, bench) only ever write #include "imu_lab/...".
target_link_libraries(imu_lab_core PUBLIC Threads::Threads)

if(WIN32)
    # GetProcessMemoryInfo lives in psapi.
    target_link_libraries(imu_lab_core PUBLIC psapi)
endif()

imu_lab_set_warnings(imu_lab_core)

# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
add_executable(imu_lab_cli src/main.cpp)
target_link_libraries(imu_lab_cli PRIVATE imu_lab_core)
imu_lab_set_warnings(imu_lab_cli)

# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #
if(IMU_LAB_BUILD_TESTS)
    enable_testing()

    add_executable(imu_lab_tests
        tests/test_main.cpp
        tests/test_csv_reader.cpp
        tests/test_jump_detector.cpp
        tests/test_metrics.cpp
        tests/test_number_parser.cpp
        tests/test_pipeline.cpp
        tests/test_protocol_parser.cpp
        tests/test_statistics.cpp
        tests/test_thread_safe_queue.cpp
    )
    target_link_libraries(imu_lab_tests PRIVATE imu_lab_core)
    target_include_directories(imu_lab_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    imu_lab_set_warnings(imu_lab_tests)

    # One CTest entry overall, plus one per suite so a failure names the area.
    add_test(NAME all COMMAND imu_lab_tests)
    foreach(suite IN ITEMS
            NumberParser ProtocolParser CsvReader JumpDetector
            Statistics ThreadSafeQueue Metrics Pipeline)
        add_test(NAME suite_${suite} COMMAND imu_lab_tests ${suite})
    endforeach()

    set_tests_properties(all PROPERTIES TIMEOUT 300)
endif()

# --------------------------------------------------------------------------- #
# Bench / stability harness
# --------------------------------------------------------------------------- #
if(IMU_LAB_BUILD_BENCH)
    add_executable(imu_lab_bench bench/bench_pipeline.cpp)
    target_link_libraries(imu_lab_bench PRIVATE imu_lab_core)
    imu_lab_set_warnings(imu_lab_bench)

    # Isolates the queue so scaling behaviour can be attributed to a component
    # rather than guessed at.
    add_executable(imu_lab_bench_queue bench/bench_queue.cpp)
    target_link_libraries(imu_lab_bench_queue PRIVATE imu_lab_core)
    imu_lab_set_warnings(imu_lab_bench_queue)

    if(IMU_LAB_BUILD_TESTS)
        # A short sweep runs as part of CTest; the CI soak job runs it much
        # bigger via the bench binary directly.
        add_test(NAME stability_sweep
                 COMMAND imu_lab_bench --frames 20000 --sweep --queue-capacity 128)
        set_tests_properties(stability_sweep PROPERTIES TIMEOUT 300)

        # The micro-benchmark also asserts that nothing is lost; it is cheap
        # enough (1M items) to keep in the normal test run.
        add_test(NAME queue_microbenchmark
                 COMMAND imu_lab_bench_queue --items 200000 --queue-capacity 1024)
        set_tests_properties(queue_microbenchmark PROPERTIES TIMEOUT 300)
    endif()
endif()

# --------------------------------------------------------------------------- #
# Summary
# --------------------------------------------------------------------------- #
message(STATUS "imu_attitude_lab: C++${CMAKE_CXX_STANDARD} with ${CMAKE_CXX_COMPILER_ID} "
               "${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  warnings=err : ${IMU_LAB_WERROR}")
message(STATUS "  sanitizer    : ${IMU_LAB_SANITIZER}")
message(STATUS "  binaries     : ${CMAKE_BINARY_DIR}/bin")
