cmake_minimum_required(VERSION 3.21)

set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "Supported configurations" FORCE)

project(orbit VERSION 0.1.4 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Orbit targets Windows x64 only.")
endif()

include(cmake/deps.cmake)

# Static CRT to match CEF's libcef_dll_wrapper defaults (/MT, /MTd).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CEF_ROOT "${ORBIT_CEF_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# The minimal CEF distribution ships Release binaries only. libcef exposes a C
# ABI, so Debug builds of our code (and of libcef_dll_wrapper) link against the
# Release libcef.dll safely.
set(CEF_LIB_DEBUG "${CEF_LIB_RELEASE}")
set(CEF_BINARY_DIR "${CEF_BINARY_DIR_RELEASE}")

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

# CEF resets the C++ defaults for Ninja; do the same for C (SQLite) so CMake's
# default /RTC1 cannot clash with optimization flags.
set(CMAKE_C_FLAGS "")
set(CMAKE_C_FLAGS_DEBUG "/Od")
set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")

set(ORBIT_OUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${ORBIT_OUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${ORBIT_OUT_DIR}")

# Compile flags for application-owned code. CEF's flag set (/W4 /WX, no
# exceptions, no RTTI, C++20) is applied through SET_LIBRARY_TARGET_PROPERTIES
# for targets that include CEF headers; orbit_core mirrors it so both halves
# share one dialect.
set(ORBIT_CXX_FLAGS /std:c++20 /permissive- /utf-8 /W4 /WX /GR- /Zi /MP
    /wd4100 /wd4324)
set(ORBIT_DEFINES UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _HAS_EXCEPTIONS=0 WINVER=0x0A00 _WIN32_WINNT=0x0A00
    ORBIT_VERSION="${PROJECT_VERSION}")

add_subdirectory(third_party_build/sqlite)
add_subdirectory(src)

option(ORBIT_BUILD_TESTS "Build unit tests" ON)
if(ORBIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

PRINT_CEF_CONFIG()
