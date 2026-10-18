cmake_minimum_required(VERSION 3.20)
project(telemetry_reader_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(telemetry_reader_example main.cpp)
target_include_directories(telemetry_reader_example PRIVATE ../../include)