cmake_minimum_required(VERSION 3.20)
project(HeadsetBatteryPlugin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(HeadsetBatteryPlugin SHARED Plugin.cpp)

target_compile_definitions(HeadsetBatteryPlugin PRIVATE UNICODE _UNICODE)

target_link_libraries(HeadsetBatteryPlugin PRIVATE kernel32)

set_target_properties(HeadsetBatteryPlugin PROPERTIES
    OUTPUT_NAME "HeadsetBatteryPlugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output"
)
