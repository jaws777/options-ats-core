cmake_minimum_required(VERSION 3.25)
project(options_ats_core_consumer LANGUAGES CXX)

find_package(options_ats_core 0.1 REQUIRED CONFIG)

add_executable(options_ats_core_consumer main.cpp)
target_link_libraries(options_ats_core_consumer PRIVATE options_ats_core::options_ats_core)

