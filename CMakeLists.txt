#
# oneJDK-25 — CLion & IDE CMake Configuration
# ============================================
# This CMakeLists.txt is designed for JetBrains CLion and modern C++ IDEs to provide
# full semantic indexing, code completion, refactoring, and Clangd support for
# HotSpot JVM, Intel oneTBB, and TableGen compiler backends.
#

cmake_minimum_required(VERSION 3.20)
project(oneJDK-25 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Include Directories for CLion Code Insight
# ============================================================================

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hotspot/share
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hotspot/share/precompiled
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hotspot/share/tbb/oneTBB-2023.2.0/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hotspot/share/tdlc/llvm_src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hotspot/cpu/x86
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hotspot/os/windows
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hotspot/os_cpu/windows_x86
    ${CMAKE_CURRENT_SOURCE_DIR}/src/java.base/share/native/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/java.base/windows/native/include
    ${CMAKE_CURRENT_SOURCE_DIR}/build/onejdk-25/gensrc/tablegen
    ${CMAKE_CURRENT_SOURCE_DIR}/build/tablegen_test
)

# ============================================================================
# Preprocessor Definitions for HotSpot & GC Systems
# ============================================================================

add_definitions(
    -D_WINDOWS
    -DWIN32
    -DHOTSPOT_BUILD_TARGET="server"
    -DCOMPILER2
    -DALL_GCS_NAMES
    -DINCLUDE_SHENANDOAHGC=1
    -DINCLUDE_ZGC=1
    -DINCLUDE_G1GC=1
    -DASSERT
    -D_CRT_SECURE_NO_WARNINGS
)

# ============================================================================
# HotSpot Source Files Indexing
# ============================================================================

file(GLOB_RECURSE HOTSPOT_SHARE_SOURCES
    "src/hotspot/share/*.cpp"
    "src/hotspot/share/*.hpp"
)

file(GLOB_RECURSE HOTSPOT_X86_SOURCES
    "src/hotspot/cpu/x86/*.cpp"
    "src/hotspot/cpu/x86/*.hpp"
)

file(GLOB_RECURSE HOTSPOT_AARCH64_SOURCES
    "src/hotspot/cpu/aarch64/*.cpp"
    "src/hotspot/cpu/aarch64/*.hpp"
)

file(GLOB_RECURSE HOTSPOT_RISCV_SOURCES
    "src/hotspot/cpu/riscv/*.cpp"
    "src/hotspot/cpu/riscv/*.hpp"
)

file(GLOB_RECURSE HOTSPOT_S390_SOURCES
    "src/hotspot/cpu/s390/*.cpp"
    "src/hotspot/cpu/s390/*.hpp"
)

file(GLOB_RECURSE TDLc_SOURCES
    "src/hotspot/share/tdlc/backend/*.cpp"
    "src/hotspot/share/tdlc/backend/*.hpp"
)

# Target for CLion project tree and Clangd indexing
add_library(onejdk_hotspot EXCLUDE_FROM_ALL
    ${HOTSPOT_SHARE_SOURCES}
    ${HOTSPOT_X86_SOURCES}
    ${HOTSPOT_AARCH64_SOURCES}
    ${HOTSPOT_RISCV_SOURCES}
    ${HOTSPOT_S390_SOURCES}
    ${TDLc_SOURCES}
)
