cmake_minimum_required(VERSION 3.16)
project(ets2_telemetry_udp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(ets2_telemetry_udp SHARED
  src/telemetry.cpp
)

target_include_directories(ets2_telemetry_udp PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

# Optional: make the output name nicer
set_target_properties(ets2_telemetry_udp PROPERTIES
  OUTPUT_NAME "ets2_telemetry_udp"
)
