set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_RK_DEFAULT_SDK_ROOT "/home/alientek/rv1126_sdk")
if(DEFINED ENV{RVSDK} AND NOT "$ENV{RVSDK}" STREQUAL "")
    set(_RK_DEFAULT_SDK_ROOT "$ENV{RVSDK}")
endif()

set(RK_SDK_ROOT "${_RK_DEFAULT_SDK_ROOT}" CACHE PATH
    "Root of the complete RV1126 Linux SDK")
set(RK_TOOLCHAIN_ROOT
    "${RK_SDK_ROOT}/prebuilts/gcc/linux-x86/arm/gcc-arm-8.3-2019.03-x86_64-arm-linux-gnueabihf"
    CACHE PATH "Root of the RV1126 arm-linux-gnueabihf GCC distribution")
set(RK_SDK_SYSROOT
    "${RK_SDK_ROOT}/buildroot/output/alientek_rv1126/host/arm-buildroot-linux-gnueabihf/sysroot"
    CACHE PATH "RV1126 Buildroot target sysroot")

if(NOT IS_DIRECTORY "${RK_TOOLCHAIN_ROOT}")
    message(FATAL_ERROR "RV1126 toolchain root does not exist: ${RK_TOOLCHAIN_ROOT}")
endif()

set(_RK_TOOL_PREFIX "${RK_TOOLCHAIN_ROOT}/bin/arm-linux-gnueabihf")
set(_RK_C_COMPILER "${_RK_TOOL_PREFIX}-gcc")
set(_RK_CXX_COMPILER "${_RK_TOOL_PREFIX}-g++")

foreach(_RK_REQUIRED_PATH IN ITEMS "${_RK_C_COMPILER}" "${_RK_CXX_COMPILER}")
    if(NOT EXISTS "${_RK_REQUIRED_PATH}")
        message(FATAL_ERROR "RV1126 cross compiler is missing: ${_RK_REQUIRED_PATH}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${RK_SDK_SYSROOT}")
    message(FATAL_ERROR "RV1126 sysroot does not exist: ${RK_SDK_SYSROOT}")
endif()

set(CMAKE_C_COMPILER "${_RK_C_COMPILER}" CACHE FILEPATH "RV1126 C compiler")
set(CMAKE_CXX_COMPILER "${_RK_CXX_COMPILER}" CACHE FILEPATH "RV1126 C++ compiler")
set(CMAKE_SYSROOT "${RK_SDK_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH "${RK_SDK_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

unset(_RK_DEFAULT_SDK_ROOT)
unset(_RK_TOOL_PREFIX)
unset(_RK_C_COMPILER)
unset(_RK_CXX_COMPILER)
unset(_RK_REQUIRED_PATH)
