# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
#
# @author Arun Madhavan

cmake_minimum_required(VERSION 3.16)
project(rdkmfrlibs VERSION 1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "rdkmfrlibs requires a Linux target. Use a Linux SDK toolchain for cross-compilation.")
endif()

include(GNUInstallDirs)
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_THERMAL_PROTECTION "Build the HAL thermal protection functions" OFF)
option(ENABLE_SINGLE_INSTANCE_LOCK "Enable the HAL single-instance lock" OFF)
option(ENABLE_NATIVE_FLASHER "Enable asynchronous native OTA flashing in the HAL" ON)
set(RDKMFRLIBS_INCLUDE_DIRS
    "${CMAKE_SYSROOT}/usr/include/rdk/iarmmgrs-hal;${CMAKE_SYSROOT}/usr/include/rdk/iarmbus"
    CACHE STRING "RDK HAL and IARM header directories")
set(RDKMFRLIBS_LIBS "" CACHE STRING "Additional libraries for the RDK MFR HAL")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# Add GIT version to the compile flags
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if (NOT GIT_SHA STREQUAL "")
    message(STATUS "GIT_SHA is set to ${GIT_SHA}")
    add_definitions(-DGIT_SHA=\"${GIT_SHA}\")
else()
    message(STATUS "GIT_SHA is not set")
endif()

add_library(RDKMfrLib SHARED mfrlibs_rpi.c BootStateLock.c BootStateLock.h)
# Preserve the previous Libtool default SONAME and library version.
set_target_properties(RDKMfrLib PROPERTIES VERSION 0.0.0 SOVERSION 0)
target_include_directories(RDKMfrLib PUBLIC ${RDKMFRLIBS_INCLUDE_DIRS})
target_link_libraries(RDKMfrLib PRIVATE ${RDKMFRLIBS_LIBS})
if(ENABLE_NATIVE_FLASHER)
    target_compile_definitions(RDKMfrLib PRIVATE USE_NATIVE_FLASHER=1)
    target_sources(RDKMfrLib PRIVATE NativeFlashBridge.cpp NativeFlashBridge.h
        FlashProcessor.hpp FlashConfig.hpp WicStreamFlasher.cpp WicStreamFlasher.hpp)
    target_link_libraries(RDKMfrLib PRIVATE PkgConfig::LIBARCHIVE Threads::Threads)
endif()
if(ENABLE_THERMAL_PROTECTION)
    target_sources(RDKMfrLib PRIVATE mfrtherm_mon.c)
endif()
if(ENABLE_SINGLE_INSTANCE_LOCK)
    target_compile_definitions(RDKMfrLib PRIVATE ENABLE_SINGLE_INSTANCE_LOCK=1)
endif()

add_executable(mfrHalUtility mfrlib_utility.c)
target_link_libraries(mfrHalUtility PRIVATE RDKMfrLib)

add_executable(FlashApp FlashApp.cpp FlashProcessor.hpp FlashConfig.hpp
    WicStreamFlasher.cpp WicStreamFlasher.hpp BootStateLock.c BootStateLock.h)
target_include_directories(FlashApp PRIVATE ${RDKMFRLIBS_INCLUDE_DIRS})
target_link_libraries(FlashApp PRIVATE PkgConfig::LIBARCHIVE Threads::Threads)

function(check_flashapp_filesystem)
    # SDKs may skip compiler link checks, but filesystem needs a real link test.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(filesystem_test "
        #include <filesystem>
        int main() { return std::filesystem::exists(\".\") ? 0 : 1; }
    ")
    cmake_push_check_state(RESET)
    check_cxx_source_compiles("${filesystem_test}" FLASHAPP_HAS_FILESYSTEM)
    if(NOT FLASHAPP_HAS_FILESYSTEM)
        set(CMAKE_REQUIRED_LIBRARIES stdc++fs)
        check_cxx_source_compiles("${filesystem_test}" FLASHAPP_HAS_STDCXXFS)
        if(NOT FLASHAPP_HAS_STDCXXFS)
            message(FATAL_ERROR "FlashApp requires C++17 filesystem support, optionally provided by libstdc++fs.")
        endif()
        target_link_libraries(FlashApp PRIVATE stdc++fs)
        if(ENABLE_NATIVE_FLASHER)
            target_link_libraries(RDKMfrLib PRIVATE stdc++fs)
        endif()
    endif()
    cmake_pop_check_state()
endfunction()
check_flashapp_filesystem()

install(TARGETS RDKMfrLib mfrHalUtility FlashApp
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
