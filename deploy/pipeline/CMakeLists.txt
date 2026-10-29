cmake_minimum_required(VERSION 3.10)

project(rk_vision_pipeline LANGUAGES C CXX)

option(RK_VISION_BUILD_DEVICE "Build the RKMedia/RGA/RockX device application" ON)
option(RK_VISION_ENABLE_ISP "Manage the camera ISP directly through RKAiq" ON)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

if(BUILD_TESTING OR RK_VISION_BUILD_DEVICE)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_executable(core_tests tests/core_tests.cpp)
    target_include_directories(core_tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_options(core_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(core_tests PRIVATE Threads::Threads)
    add_test(NAME core_tests COMMAND core_tests)

    add_executable(decode_tests ../test_decode.cpp)
    target_include_directories(decode_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/.."
        "${PROJECT_SOURCE_DIR}/include")
    target_compile_options(decode_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME decode_tests COMMAND decode_tests)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# A host-only configure must not inspect or link any target SDK artifact.
if(NOT RK_VISION_BUILD_DEVICE)
    if(BUILD_TESTING)
        message(STATUS "RK device application disabled; only portable host tests are enabled")
    else()
        message(STATUS "RK device application and tests are disabled; no build targets were requested")
    endif()
endif()

if(RK_VISION_BUILD_DEVICE)

set(RK_SDK_SYSROOT "${CMAKE_SYSROOT}" CACHE PATH
    "Sysroot containing the target RKMedia, RGA, RockX and optional RKAiq SDK")
if(NOT RK_SDK_SYSROOT)
    message(FATAL_ERROR
        "RK_VISION_BUILD_DEVICE=ON requires RK_SDK_SYSROOT or a toolchain file that sets "
        "CMAKE_SYSROOT. For RV1126 use -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1126.cmake, "
        "or configure a host-only build with -DRK_VISION_BUILD_DEVICE=OFF.")
endif()
if(NOT IS_DIRECTORY "${RK_SDK_SYSROOT}/usr/include")
    message(FATAL_ERROR "RK SDK include directory is missing: ${RK_SDK_SYSROOT}/usr/include")
endif()

set(RK_SDK_INCLUDE_HINTS
    "${RK_SDK_SYSROOT}/usr/include"
    "${RK_SDK_SYSROOT}/usr/include/easymedia"
    "${RK_SDK_SYSROOT}/usr/include/rkmedia"
    "${RK_SDK_SYSROOT}/usr/include/rga"
    "${RK_SDK_SYSROOT}/usr/include/rockx")
set(RK_SDK_LIBRARY_HINTS
    "${RK_SDK_SYSROOT}/usr/lib"
    "${RK_SDK_SYSROOT}/lib"
    "${RK_SDK_SYSROOT}/usr/lib/arm-linux-gnueabihf"
    "${RK_SDK_SYSROOT}/lib/arm-linux-gnueabihf")

function(rk_find_required_path output header description)
    # All hints below are already absolute paths inside RK_SDK_SYSROOT.
    # Avoid re-rooting them to <sysroot>/<sysroot>/... in cross builds.
    find_path(${output} NAMES "${header}" PATHS ${ARGN}
              NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT ${output})
        message(FATAL_ERROR "Missing ${description} header '${header}' under RK_SDK_SYSROOT=${RK_SDK_SYSROOT}")
    endif()
    set(${output} "${${output}}" PARENT_SCOPE)
endfunction()

function(rk_find_required_library output library description)
    find_library(${output} NAMES "${library}" PATHS ${RK_SDK_LIBRARY_HINTS}
                 NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT ${output})
        message(FATAL_ERROR "Missing ${description} library '${library}' under RK_SDK_SYSROOT=${RK_SDK_SYSROOT}")
    endif()
    set(${output} "${${output}}" PARENT_SCOPE)
endfunction()

rk_find_required_path(RKMEDIA_INCLUDE_DIR rkmedia_api.h "RKMedia" ${RK_SDK_INCLUDE_HINTS})
rk_find_required_path(RGA_INCLUDE_DIR im2d.h "RGA im2d" ${RK_SDK_INCLUDE_HINTS})
rk_find_required_library(EASYMEDIA_LIBRARY easymedia "RKMedia/easymedia")
rk_find_required_library(RGA_LIBRARY rga "RGA")
rk_find_required_library(ALSA_LIBRARY asound "ALSA")

set(RK_ISP_INCLUDE_DIRS)
set(RK_ISP_LIBRARIES)
if(RK_VISION_ENABLE_ISP)
    rk_find_required_path(RKAIQ_SYSCTL_INCLUDE_DIR rk_aiq_user_api_sysctl.h "RKAiq sysctl"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq/uAPI"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq")
    rk_find_required_path(RKAIQ_IMGPROC_INCLUDE_DIR rk_aiq_user_api_imgproc.h "RKAiq image processing"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq/uAPI"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq")
    rk_find_required_library(RKAIQ_LIBRARY rkaiq "RKAiq")
    list(APPEND RK_ISP_INCLUDE_DIRS
        "${RKAIQ_SYSCTL_INCLUDE_DIR}"
        "${RKAIQ_IMGPROC_INCLUDE_DIR}"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq/uAPI"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq/xcore"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq/algos"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq/common"
        "${RK_SDK_SYSROOT}/usr/include/rkaiq/iq_parser")
    list(APPEND RK_ISP_LIBRARIES "${RKAIQ_LIBRARY}")
endif()

set(RK_VISION_DEVICE_SOURCES
    rkmedia_rockx_face_detection_rtsp.cpp
    src/pipeline.cpp
    src/rk_backend.cpp
    src/rtsp_server.cpp)
foreach(source IN LISTS RK_VISION_DEVICE_SOURCES)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "Missing device source file: ${PROJECT_SOURCE_DIR}/${source}")
    endif()
endforeach()

add_executable(rkmedia_rockx_face_detection_rtsp ${RK_VISION_DEVICE_SOURCES})
set_target_properties(rkmedia_rockx_face_detection_rtsp PROPERTIES OUTPUT_NAME rkmedia_object_detection_rtsp)
target_compile_options(rkmedia_rockx_face_detection_rtsp PRIVATE -Wall -Wextra -Wpedantic)
set(RKNN_HEADER_DIR "${RK_SDK_ROOT}/external/rknpu/rknn/rknn_api/librknn_api/include")
rk_find_required_library(RKNN_LIBRARY rknn_api "first-generation RKNN")
rk_find_required_library(OPENCV_CORE opencv_core "OpenCV core")
rk_find_required_library(OPENCV_IMGPROC opencv_imgproc "OpenCV imgproc")
rk_find_required_library(OPENCV_IMGCODECS opencv_imgcodecs "OpenCV imgcodecs")
add_library(object_detector STATIC ../objects.cpp)
target_include_directories(object_detector PUBLIC .. "${PROJECT_SOURCE_DIR}/include" "${RKNN_HEADER_DIR}")
target_link_libraries(object_detector PUBLIC "${RKNN_LIBRARY}" "${OPENCV_CORE}" "${OPENCV_IMGPROC}" "${OPENCV_IMGCODECS}")
add_executable(object_image ../image_main.cpp)
target_link_libraries(object_image PRIVATE object_detector)
target_include_directories(rkmedia_rockx_face_detection_rtsp PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}")
target_include_directories(rkmedia_rockx_face_detection_rtsp SYSTEM PRIVATE
    "${RK_SDK_SYSROOT}/usr/include"
    "${RKMEDIA_INCLUDE_DIR}"
    "${RGA_INCLUDE_DIR}"
    ${RK_ISP_INCLUDE_DIRS})
if(RK_VISION_ENABLE_ISP)
    target_compile_definitions(rkmedia_rockx_face_detection_rtsp PRIVATE RKAIQ=1)
endif()
target_link_libraries(rkmedia_rockx_face_detection_rtsp PRIVATE
    object_detector
    "${EASYMEDIA_LIBRARY}"
    "${RGA_LIBRARY}"
    "${ALSA_LIBRARY}"
    ${RK_ISP_LIBRARIES}
    Threads::Threads
    ${CMAKE_DL_LIBS})

message(STATUS "RK device target enabled (sysroot: ${RK_SDK_SYSROOT}, ISP: ${RK_VISION_ENABLE_ISP})")
endif()
