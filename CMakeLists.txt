cmake_minimum_required(VERSION 3.15)
project(ets2la_telemetry VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ets2la_telemetry SHARED
    src/dll/dllmain.cpp
    src/dll/telemetry_plugin.cpp
)

target_include_directories(ets2la_telemetry PRIVATE include)
set_target_properties(ets2la_telemetry PROPERTIES OUTPUT_NAME "ets2la_telemetry")