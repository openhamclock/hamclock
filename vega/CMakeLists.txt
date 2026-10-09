cmake_minimum_required(VERSION 3.20)
project(HamClockVegaNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ESPHamClock")

add_compile_definitions(
    _WEB_ONLY
    _CLOCK_1600x960
    NO_UPGRADE
    NO_SYSTEM_CONTROLS
    _IS_LINUX
    VEGA_OS
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/native
    ${HC_DIR}
    ${HC_DIR}/ArduinoLib
    ${HC_DIR}/wsServer/include
    ${HC_DIR}/zlib-hc
)

file(GLOB HC_SRCS CONFIGURE_DEPENDS
    "${HC_DIR}/*.cpp"
)

file(GLOB ARDUINO_SRCS CONFIGURE_DEPENDS
    "${HC_DIR}/ArduinoLib/*.cpp"
)

file(GLOB WS_SRCS CONFIGURE_DEPENDS
    "${HC_DIR}/wsServer/src/*.cpp"
)

file(GLOB ZLIB_SRCS CONFIGURE_DEPENDS
    "${HC_DIR}/zlib-hc/*.cpp"
)

set(TURBO_SRCS
    native/HamClockTurboModule.cpp
    ${HC_SRCS}
    ${ARDUINO_SRCS}
    ${WS_SRCS}
    ${ZLIB_SRCS}
)

# Use Vega/Kepler TurboModule macro if available in Vega SDK, otherwise fallback to standard CMake SHARED lib
if(COMMAND kepler_add_turbo_module_library)
    kepler_add_turbo_module_library(
        HamClockTurboModule
        SOURCES ${TURBO_SRCS}
    )
else()
    add_library(
        HamClockTurboModule
        SHARED
        ${TURBO_SRCS}
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(HamClockTurboModule PRIVATE Threads::Threads)
