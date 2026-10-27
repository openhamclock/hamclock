cmake_minimum_required(VERSION 3.15)

project(HcEngine
        VERSION 1.0
        LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fail at link time for unresolved symbols.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_link_options("LINKER:--no-undefined" "LINKER:-z,defs")
endif()

# ---------------------------------------------------------------------------
# HamClock engine (web-only build), compiled straight from ../../ESPHamClock.
# Source lists are read from HamClock's own Makefiles so this stays in sync.
# ---------------------------------------------------------------------------
get_filename_component(HC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ESPHamClock" ABSOLUTE)
if(NOT EXISTS "${HC_DIR}/Makefile")
  message(FATAL_ERROR "HamClock sources not found at ${HC_DIR}")
endif()

# hc_makefile_sources(<makefile> <variable> <out>): expand "VAR = a.o b.o \" lists into
# existing .cpp/.c paths next to the Makefile.
function(hc_makefile_sources makefile var out)
  file(READ "${makefile}" text)
  string(REGEX MATCH "\n${var}[ \t]*=[ \t]*([^\n]*\\\\\n)*[^\n]*" block "\n${text}")
  if(NOT block)
    message(FATAL_ERROR "${var} not found in ${makefile}")
  endif()
  string(REGEX REPLACE "^\n${var}[ \t]*=" "" block "${block}")
  string(REPLACE "\\\n" " " block "${block}")
  separate_arguments(items UNIX_COMMAND "${block}")
  get_filename_component(dir "${makefile}" DIRECTORY)
  set(result)
  foreach(item ${items})
    string(REGEX REPLACE "\\.(o|cpp|c)$" "" stem "${item}")
    if(EXISTS "${dir}/${stem}.cpp")
      list(APPEND result "${dir}/${stem}.cpp")
    elseif(EXISTS "${dir}/${stem}.c")
      list(APPEND result "${dir}/${stem}.c")
    else()
      message(FATAL_ERROR "${makefile}: no source for ${item}")
    endif()
  endforeach()
  set(${out} ${result} PARENT_SCOPE)
endfunction()

hc_makefile_sources("${HC_DIR}/Makefile" OBJS HC_MAIN_SRCS)
hc_makefile_sources("${HC_DIR}/ArduinoLib/Makefile" OBJS HC_ARDUINO_SRCS)
hc_makefile_sources("${HC_DIR}/wsServer/Makefile" SRC HC_WS_SRCS)
hc_makefile_sources("${HC_DIR}/zlib-hc/Makefile" LIBOBJ HC_ZLIB_SRCS)
set(HC_ENGINE_SRCS ${HC_MAIN_SRCS} ${HC_ARDUINO_SRCS} ${HC_WS_SRCS} ${HC_ZLIB_SRCS})
list(LENGTH HC_ENGINE_SRCS HC_ENGINE_COUNT)
message(STATUS "HamClock engine: ${HC_ENGINE_COUNT} sources from ${HC_DIR}")

# Same as `make hamclock-web-1600x960`, plus the switches for an app-store build.
add_library(hamclock_engine OBJECT ${HC_ENGINE_SRCS}
    kepler/engine/NvSeed.cpp)   # host code that needs HamClock.h, so built with the engine flags
set_source_files_properties(${HC_ENGINE_SRCS} PROPERTIES LANGUAGE CXX)
target_include_directories(hamclock_engine PRIVATE
    ${HC_DIR} ${HC_DIR}/ArduinoLib ${HC_DIR}/wsServer/include ${HC_DIR}/zlib-hc)
target_compile_definitions(hamclock_engine PRIVATE
    _WEB_ONLY _CLOCK_1600x960   # web server only, 1600x960 canvas
    NO_UPGRADE                  # updates come from the Appstore
    NO_SYSTEM_CONTROLS          # no OS shutdown/reboot from the clock
    _IS_VEGA)
target_compile_options(hamclock_engine PRIVATE -w -pthread)  # upstream code; don't drown our warnings
set_target_properties(hamclock_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden    # keep engine symbols out of the app's global namespace
    VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# Turbo Module
# ---------------------------------------------------------------------------
set(HEADERS
  kepler/turbo-modules/generated/HcEngineSpec.h
  kepler/turbo-modules/HcEngine.h
  kepler/engine/EngineHost.h
)
set(SOURCES
    kepler/turbo-modules/generated/HcEngineSpec.cpp
    kepler/turbo-modules/HcEngine.cpp
    kepler/engine/EngineHost.cpp
    kepler/engine/SysWraps.cpp
    kepler/AutoLinkInit.cpp
)

find_package(turbomoduleAPI CONFIG REQUIRED)

kepler_add_turbo_module_library(HcEngine SHARED ${HEADERS} ${SOURCES})

target_link_libraries(HcEngine PRIVATE hamclock_engine pthread m)

# Contain HamClock's process-level calls; see kepler/engine/SysWraps.cpp.
target_link_options(HcEngine PRIVATE
    "LINKER:--wrap=exit"
    "LINKER:--wrap=fork"
    "LINKER:--wrap=bind"
    "LINKER:--wrap=accept"
    "LINKER:--wrap=close"
    "LINKER:--wrap=getrusage"
    "LINKER:--wrap=pthread_create")

install(TARGETS HcEngine)
