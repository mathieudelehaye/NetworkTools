cmake_minimum_required(VERSION 3.15)

# ---------------------------------------------------------------------------
# vcpkg toolchain — MUST be set before project(), otherwise CMake has already
# run its compiler/package detection and manifest mode never triggers.
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    else()
        set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    endif()
endif()

if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "vcpkg toolchain not found at:\n  ${CMAKE_TOOLCHAIN_FILE}\n"
        "Clone and bootstrap vcpkg, or pass -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg.cmake")
endif()

# Static triplet => the resulting .so is self-contained; no LD_LIBRARY_PATH
# juggling when Streamlit imports it. Override with -DVCPKG_TARGET_TRIPLET=...
if(NOT DEFINED VCPKG_TARGET_TRIPLET AND UNIX AND NOT APPLE)
    set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "vcpkg triplet")
endif()

project(cpp_video_engine LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # required: static OpenCV -> shared module

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ENGINE_NATIVE_ARCH "Compile with -march=native (fastest, not portable)" ON)
option(ENGINE_PLAIN_SO_NAME
       "Emit cpp_video_engine.so instead of cpp_video_engine.cpython-3XX-....so" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# Development.Module (not Development) is correct for extension modules: it asks
# for headers only, no libpython to link against.
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

# pybind11 comes from the Python environment, NOT from vcpkg.
#
# The vcpkg pybind11 port declares a dependency on the python3 port, which
# builds an entire CPython from source (dragging in openssl, sqlite3, libffi,
# libb2, liblzma, expat, bzip2 ...) purely so the port can find an interpreter.
# We already have the interpreter we care about — the venv one — and pybind11
# is header-only, so that build is pure waste and an extra failure surface.
# The pip package ships the same headers plus a CMake config.
if(NOT pybind11_DIR)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
        OUTPUT_VARIABLE _pybind11_cmakedir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _pybind11_found
        ERROR_QUIET)
    if(_pybind11_found EQUAL 0)
        set(pybind11_DIR "${_pybind11_cmakedir}" CACHE PATH "pybind11 CMake config" FORCE)
    else()
        message(FATAL_ERROR
            "pybind11 not available to ${Python3_EXECUTABLE}.\n"
            "Install it into the same environment:  pip install pybind11")
    endif()
endif()

find_package(pybind11 CONFIG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc)

message(STATUS "Python3     : ${Python3_EXECUTABLE} (${Python3_VERSION})")
message(STATUS "OpenCV      : ${OpenCV_VERSION}")
message(STATUS "pybind11    : ${pybind11_VERSION}")

# ---------------------------------------------------------------------------
# The extension module
# ---------------------------------------------------------------------------
pybind11_add_module(cpp_video_engine MODULE native_engine.cpp)

target_link_libraries(cpp_video_engine PRIVATE ${OpenCV_LIBS})
target_include_directories(cpp_video_engine PRIVATE ${OpenCV_INCLUDE_DIRS})

target_compile_options(cpp_video_engine PRIVATE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    -Wall -Wextra)

if(ENGINE_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
    if(HAS_MARCH_NATIVE)
        target_compile_options(cpp_video_engine PRIVATE -march=native)
    endif()
endif()

if(ENGINE_PLAIN_SO_NAME)
    set_target_properties(cpp_video_engine PROPERTIES SUFFIX ".so")
endif()

# ---------------------------------------------------------------------------
# Drop the built module straight next to app.py
# ---------------------------------------------------------------------------
get_filename_component(PROJECT_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
set(PYTHON_SERVER_DIR "${PROJECT_SRC_ROOT}/python-server"
    CACHE PATH "Where the compiled module is written")

file(MAKE_DIRECTORY "${PYTHON_SERVER_DIR}")

set_target_properties(cpp_video_engine PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PYTHON_SERVER_DIR}")
# Multi-config generators ignore the plain variable, so set the per-config ones too.
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(cpp_video_engine PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${cfg} "${PYTHON_SERVER_DIR}")
endforeach()

message(STATUS "Module output -> ${PYTHON_SERVER_DIR}")