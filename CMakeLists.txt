cmake_minimum_required(VERSION 3.13.4)

project(TypeCorrect VERSION 0.0.1 LANGUAGES CXX)
string(TOLOWER "${PROJECT_NAME}" PROJECT_LOWER_NAME)
string(REPLACE "-" "_" PROJECT_LOWER_NAME "${PROJECT_LOWER_NAME}")

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# -fvisibility-inlines-hidden is set when building LLVM and on Darwin warnings
# are triggered if type-correct is built without this flag (though otherwise it
# builds fine). For consistency, add it here too.
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

#===============================================================================
# 0. GET CLANG INSTALLATION DIR
#===============================================================================
# In clang-tutor, `CT_Clang_INSTALL_DIR` is the key CMake variable - it points
# to a Clang installation directory. For the sake of completeness,
# <PackageName>_DIR (i.e. `Clang_DIR`) and <PackageName>_ROOT (i.e.
# `Clang_ROOT`) are also supported. Visit CMake documentation for more details:
#   https://cmake.org/cmake/help/latest/command/find_package.html
# Use only _one_ of these variables.

if(DEFINED Clang_ROOT)
    set(CT_CLANG_PACKAGE_DIR "${Clang_ROOT}/../../..")
elseif(DEFINED Clang_DIR)
    set(CT_CLANG_PACKAGE_DIR "${Clang_DIR}/../../..")
endif()
mark_as_advanced(CT_CLANG_PACKAGE_DIR)

# Set this to a valid Clang installation directory. This is most likely where
# LLVM is installed on your system.
set(CT_Clang_INSTALL_DIR "${CT_CLANG_PACKAGE_DIR}" CACHE PATH
        "Clang installation directory")

#===============================================================================
# 1. VERIFY CLANG INSTALLATION DIR
#===============================================================================
set(CT_LLVM_INCLUDE_DIR "${CT_Clang_INSTALL_DIR}/include/llvm")
if(NOT EXISTS "${CT_LLVM_INCLUDE_DIR}")
    message(FATAL_ERROR
            " CT_Clang_INSTALL_DIR (${CT_LLVM_INCLUDE_DIR}) is invalid.")
endif()

set(CT_LLVM_CMAKE_FILE
        "${CT_Clang_INSTALL_DIR}/lib/cmake/clang/ClangConfig.cmake")
if(NOT EXISTS "${CT_LLVM_CMAKE_FILE}")
    message(FATAL_ERROR
            " CT_LLVM_CMAKE_FILE (${CT_LLVM_CMAKE_FILE}) is invalid.")
endif()

#===============================================================================
# 2. LOAD CLANG CONFIGURATION
#    Extracted from:
#    http://llvm.org/docs/CMake.html#embedding-llvm-in-your-project
#===============================================================================
list(APPEND CMAKE_PREFIX_PATH "${CT_Clang_INSTALL_DIR}/lib/cmake/clang/")

find_package(Clang REQUIRED CONFIG)

# Sanity check. As Clang does not expose e.g. `CLANG_VERSION_MAJOR` through
# AddClang.cmake, we have to use LLVM_VERSION_MAJOR instead.
# TODO: Revisit when next version is released.
if(NOT "13" VERSION_EQUAL "${LLVM_VERSION_MAJOR}")
    message(FATAL_ERROR "Found LLVM ${LLVM_VERSION_MAJOR}, but need LLVM 13")
endif()

message(STATUS "Found Clang ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using ClangConfig.cmake in: ${CT_Clang_INSTALL_DIR}")

message("CLANG STATUS:
  Includes (clang)    ${CLANG_INCLUDE_DIRS}
  Includes (llvm)     ${LLVM_INCLUDE_DIRS}")

# Set the LLVM and Clang header and library paths
include_directories(SYSTEM "${LLVM_INCLUDE_DIRS};${CLANG_INCLUDE_DIRS}")

#===============================================================================
# 3. CLANG-TUTOR BUILD CONFIGURATION
#===============================================================================
# Use the same C++ standard as LLVM does
set(CMAKE_CXX_STANDARD 14 CACHE STRING "")

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE
            STRING "Build type (default Debug):" FORCE)
endif()

# Compiler flags
add_library("${PROJECT_LOWER_NAME}_compiler_flags" INTERFACE)
target_compile_features("${PROJECT_LOWER_NAME}_compiler_flags" INTERFACE "cxx_std_${CMAKE_CXX_STANDARD}")

set(gcc_like "$<COMPILE_LANG_AND_ID:C,CXX,ARMClang,AppleClang,Clang,GNU>")
set(msvc "$<COMPILE_LANG_AND_ID:C,CXX,MSVC>")
# -g -fsanitize=address -fno-omit-frame-pointer
# On Darwin (unlike on Linux), undefined symbols in shared objects are not
# allowed at the end of the link-edit. The plugins defined here:
#  - _are_ shared objects
#  - reference symbols from LLVM shared libraries, i.e. symbols which are
#    undefined until those shared objects are loaded in memory (and hence
#    _undefined_ during static linking)
# The build will fail with errors like this:
#    "Undefined symbols for architecture x86_64"
# with various LLVM symbols being undefined. Since those symbols are later
# loaded and resolved at runtime, these errors are false positives.
# This behaviour can be modified via the '-undefined' OS X linker flag as
# follows.

target_compile_options(
        "${PROJECT_LOWER_NAME}_compiler_flags"
        INTERFACE
        "$<${gcc_like}:$<BUILD_INTERFACE:-Wall;-Wextra;-Wshadow;-Wformat=2;-Wunused;-fdiagnostics-color=always>"
        "$<PLATFORM_ID:Darwin>:-undefined dynamic_lookup>"
        "$<${msvc}:$<BUILD_INTERFACE:-W3;-WX;-Zi;-permissive->>"
)
if (CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
    set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif()

# LLVM/Clang is normally built without RTTI. Be consistent with that.
if(NOT LLVM_ENABLE_RTTI)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

# Set the build directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

#===============================================================================
# 4. ADD SUB-TARGETS
# Doing this at the end so that all definitions and link/include paths are
# available for the sub-projects.
#===============================================================================
add_subdirectory("type_correct")
