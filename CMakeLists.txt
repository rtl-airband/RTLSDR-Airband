cmake_minimum_required (VERSION 3.1...3.18 FATAL_ERROR)
project (RTLSDR-Airband CXX)

execute_process(COMMAND ${PROJECT_SOURCE_DIR}/scripts/find_version
   OUTPUT_VARIABLE RTL_AIRBAND_VERSION
   OUTPUT_STRIP_TRAILING_WHITESPACE
   ERROR_VARIABLE RTL_AIRBAND_VERSION_ERROR
   ERROR_STRIP_TRAILING_WHITESPACE)

string(COMPARE EQUAL "${RTL_AIRBAND_VERSION}" "" RTL_AIRBAND_VERSION_UNSET)

if(RTL_AIRBAND_VERSION_UNSET)
   message(FATAL_ERROR "Failed to detect RTL_AIRBAND_VERSION - \"${RTL_AIRBAND_VERSION_ERROR}\"")
endif()

set (CMAKE_CXX_STANDARD 11)
set (CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_COMPILE_WARNING_AS_ERROR ON)

if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE Release)
   message(STATUS "Build type not specified: defaulting to Release")
endif(NOT CMAKE_BUILD_TYPE)

# TODO: flags to add: -Wfloat-equal -Wconversion -Wstrict-overflow=5 -Waggregate-return -Wpedantic -Wcast-align
# TODO: these could be added except for gtest: -Wswitch-enum -Wundef -Wswitch-default
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wshadow -Wdate-time -Wpointer-arith -Wwrite-strings -Wcast-qual  -Wunreachable-code -Werror")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Og -DDEBUG")

if(DEBUG_SQUELCH)
   set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG_SQUELCH")
endif()

add_subdirectory (src)
