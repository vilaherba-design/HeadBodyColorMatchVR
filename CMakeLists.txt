cmake_minimum_required(VERSION 3.25)

# Fallout 4 VR 1.2.72 port of Head Body Color Match 1.1.0, built on CommonLibF4VR (VR only).
if(NOT DEFINED ENV{VCPKG_ROOT})
	message(FATAL_ERROR "VCPKG_ROOT is not set")
endif()
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

project(HeadBodyColorMatch VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

# CommonLibF4VR configured for the VR runtime only (as F4VR-CommonFramework does).
set(F4SE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_F4 OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_NG OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_VR ON CACHE BOOL "" FORCE)
set(COMMONLIBF4VR_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/CommonLibF4VR" CACHE PATH "CommonLibF4VR checkout")
add_subdirectory("${COMMONLIBF4VR_PATH}/CommonLibF4" CommonLibF4 EXCLUDE_FROM_ALL)

add_library("${PROJECT_NAME}" SHARED "src/main.cpp")
target_include_directories("${PROJECT_NAME}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_precompile_headers("${PROJECT_NAME}" PRIVATE "src/pch.h")
target_compile_definitions("${PROJECT_NAME}" PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options("${PROJECT_NAME}" PRIVATE /W3 /utf-8 /Zc:preprocessor /permissive-)
target_link_libraries("${PROJECT_NAME}" PRIVATE CommonLibF4::CommonLibF4)
target_link_options("${PROJECT_NAME}" PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
