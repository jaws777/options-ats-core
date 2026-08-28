cmake_minimum_required(VERSION 3.25)
project(options_ats_core
    VERSION 0.1.0
    DESCRIPTION "C++20 header-only infrastructure for a listed options Alternative Trading System (ATS)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------- Options ----------
option(OPTIONS_ATS_CORE_BUILD_GTESTS     "Build GoogleTest-based test suite"  ${PROJECT_IS_TOP_LEVEL})
option(OPTIONS_ATS_CORE_BUILD_CATCH2     "Build Catch2-based test suite"      ${PROJECT_IS_TOP_LEVEL})
option(OPTIONS_ATS_CORE_BUILD_BENCHMARKS "Build latency benchmarks"           ${PROJECT_IS_TOP_LEVEL})
option(OPTIONS_ATS_CORE_BUILD_EXAMPLES   "Build example binaries"             ${PROJECT_IS_TOP_LEVEL})

# ---------- Compiler warnings ----------
# Keep strict warnings on project-owned targets only (not third-party deps).
if(MSVC)
    set(OPTIONS_ATS_CORE_WARNING_FLAGS /W4 /permissive-)
else()
    set(OPTIONS_ATS_CORE_WARNING_FLAGS
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Wno-sign-conversion
    )
endif()

# Sanitizer build: -fsanitize=address,undefined for all test targets.
# Usage: cmake -B build-asan -DCMAKE_BUILD_TYPE=Asan
set(CMAKE_CXX_FLAGS_ASAN "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_ASAN "-fsanitize=address,undefined")

# ThreadSanitizer: for concurrency tests (SPSC, seqlock, pre-trade risk).
# Usage: cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan
set(CMAKE_CXX_FLAGS_TSAN "-O1 -g -fsanitize=thread -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_TSAN "-fsanitize=thread")

# ---------- FetchContent dependencies ----------
include(FetchContent)
include(CTest)

# --- Catch2 ---
if(OPTIONS_ATS_CORE_BUILD_CATCH2 OR OPTIONS_ATS_CORE_BUILD_BENCHMARKS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        # v3.7.1, pinned to an immutable commit for reproducible builds.
        GIT_TAG        fa43b77429ba76c462b1898d6cd2f2d7a9416b14
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
endif()

# --- GoogleTest ---
if(OPTIONS_ATS_CORE_BUILD_GTESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        # v1.15.2, pinned to an immutable commit for reproducible builds.
        GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59
    )
    # Prevent GTest from overriding compiler/linker settings on Windows.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
endif()

# ---------- Header-only library ----------
add_library(options_ats_core INTERFACE)
add_library(options_ats_core::options_ats_core ALIAS options_ats_core)
target_include_directories(options_ats_core INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(options_ats_core INTERFACE cxx_std_20)

add_library(options_ats_core_warnings INTERFACE)
target_compile_options(options_ats_core_warnings INTERFACE ${OPTIONS_ATS_CORE_WARNING_FLAGS})

# ---------- Catch2 Tests ----------
if(OPTIONS_ATS_CORE_BUILD_CATCH2)
    add_executable(unit_tests
        tests/unit/test_fixed_price.cpp
        tests/unit/test_lmax_disruptor.cpp
        tests/unit/test_opra.cpp
        tests/unit/test_series_book.cpp
        tests/unit/test_spsc_queue.cpp
        tests/unit/test_order_book.cpp
        tests/unit/test_order_state_machine.cpp
        tests/unit/test_eq_engine.cpp
        tests/unit/test_pre_trade.cpp
        tests/unit/test_infra_extras.cpp
        tests/unit/test_fix_parser.cpp
        tests/unit/test_spread_state_machine.cpp
        tests/unit/test_options_types.cpp
    )
    target_link_libraries(unit_tests PRIVATE
        options_ats_core
        options_ats_core_warnings
        Catch2::Catch2WithMain
    )
    catch_discover_tests(unit_tests)
endif()

# ---------- GoogleTest Tests ----------
# GTest suite mirrors the Catch2 suite and demonstrates framework parity:
# both Catch2 and GoogleTest are supported for unit tests.
if(OPTIONS_ATS_CORE_BUILD_GTESTS)
    add_executable(gtest_suite
        tests/gtest/gtest_fixed_price.cpp
        tests/gtest/gtest_series_book.cpp
        tests/gtest/gtest_order_state_machine.cpp
        tests/gtest/gtest_pre_trade.cpp
        tests/gtest/gtest_seqlock.cpp
        tests/gtest/gtest_wal.cpp
    )
    target_link_libraries(gtest_suite PRIVATE
        options_ats_core
        options_ats_core_warnings
        GTest::gtest_main
    )
    gtest_discover_tests(gtest_suite)
endif()

# ---------- Integration Tests ----------
if(OPTIONS_ATS_CORE_BUILD_CATCH2)
    add_executable(integration_tests
        tests/integration/test_end_to_end.cpp
    )
    target_link_libraries(integration_tests PRIVATE
        options_ats_core
        options_ats_core_warnings
        Catch2::Catch2WithMain
    )
    catch_discover_tests(integration_tests)
endif()

# ---------- Benchmarks ----------
# Latency regression tests are TSC-based percentile benchmarks. CI should
# gate on p99 regression greater than 5%.
if(OPTIONS_ATS_CORE_BUILD_BENCHMARKS)
    add_executable(benchmarks
        tests/benchmarks/bench_spsc_queue.cpp
        tests/benchmarks/bench_latency.cpp
    )
    target_link_libraries(benchmarks PRIVATE
        options_ats_core
        options_ats_core_warnings
        Catch2::Catch2WithMain
    )
    target_compile_options(benchmarks PRIVATE -O3 -march=native)
endif()

# ---------- Examples ----------
if(OPTIONS_ATS_CORE_BUILD_EXAMPLES)
    add_executable(lmax_disruptor_example examples/lmax_disruptor_example.cpp)
    target_link_libraries(lmax_disruptor_example PRIVATE
        options_ats_core
        options_ats_core_warnings
    )

    add_executable(opra_consumer_example examples/opra_consumer_example.cpp)
    target_link_libraries(opra_consumer_example PRIVATE
        options_ats_core
        options_ats_core_warnings
    )
endif()

# ---------- Installation ----------
install(TARGETS options_ats_core EXPORT options_ats_coreTargets)
install(
    DIRECTORY src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)
install(
    EXPORT options_ats_coreTargets
    FILE options_ats_coreTargets.cmake
    NAMESPACE options_ats_core::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/options_ats_core
)

configure_package_config_file(
    cmake/options_ats_coreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/options_ats_coreConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/options_ats_core
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/options_ats_coreConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/options_ats_coreConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/options_ats_coreConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/options_ats_core
)
install(
    FILES README.md LICENSE NOTICE THIRD_PARTY_LICENSES.md CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/options_ats_core
)
