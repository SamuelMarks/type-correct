#===============================================================================
# FILE: CMakeLists.txt
#
# DESCRIPTION:
#   Root CMake configuration for the type-correct project. This file handles:
#   1. Project metadata and versioning.
#   2. C++ standard compliance (C++20).
#   3. Discovery and validation of the LLVM/Clang dependency (Version 16+).
#   4. Global compiler flags and visibility settings.
#   5. Sub-project generation and packaging setup.
#
# USAGE:
#   cmake -S . -B build -DCT_Clang_INSTALL_DIR=/path/to/llvm
#
# ARGUMENTS:
#   CT_Clang_INSTALL_DIR - Path to the specific LLVM/Clang installation.
#===============================================================================

cmake_minimum_required(VERSION 3.20)

# Project Definition
# ------------------
# Sets the project name, version, and enabled languages.
project(TypeCorrect VERSION 0.0.1 LANGUAGES C CXX)

# Variable: PROJECT_UNDER_NAME
# Description: Defines the snake_case name used for library and directory naming conventions.
set(PROJECT_UNDER_NAME "type_correct")

# C++ standard
set(CMAKE_CXX_STANDARD 17)

# Visibility Settings
# -------------------
# Hide symbols by default to reduce binary size and collision probability, 
# exposing only explicitly exported, public API symbols.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Variable: CMAKE_VISIBILITY_INLINES_HIDDEN
# Description: On Darwin/macOS, LLVM is built with this flag. Matching it avoids 
# visibility mismatch warnings during linking.
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

# Compile Commands
# ----------------
# Variable: CMAKE_EXPORT_COMPILE_COMMANDS
# Description: Generates 'compile_commands.json'. This is required for LibTooling 
# based tools (including this project itself) to parse source code accurately.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#===============================================================================
# SECTION 0: GET CLANG INSTALLATION DIR
#===============================================================================
# Description:
#   Locates the Clang installation directory. It prioritizes explicit CMake
#   variables (Clang_ROOT, Clang_DIR) or LLVM-specific variables.
#===============================================================================

if (DEFINED Clang_ROOT)
    # Case: Clang_ROOT is defined (Standard CMake hint)
    set(CT_CLANG_PACKAGE_DIR "${Clang_ROOT}/../../..")
elseif (DEFINED Clang_DIR)
    # Case: Clang_DIR is defined (Config package location)
    set(CT_CLANG_PACKAGE_DIR "${Clang_DIR}/../../..")
elseif (DEFINED LLVM_CMAKE_DIR)
    # Case: LLVM_CMAKE_DIR is defined (LLVM specific hint)
    set(CT_CLANG_PACKAGE_DIR "${LLVM_CMAKE_DIR}/../../..")
endif ()

# Mark internal package directory as advanced to hide from standard GUI views
mark_as_advanced(CT_CLANG_PACKAGE_DIR)

# Variable: CT_Clang_INSTALL_DIR
# Description: The canonical path to the LLVM/Clang installation root.
#              Stored in cache to allow user override via CLI.
set(CT_Clang_INSTALL_DIR "${CT_CLANG_PACKAGE_DIR}" CACHE PATH
        "Clang installation directory")

#===============================================================================
# SECTION 1: VERIFY CLANG INSTALLATION DIR
#===============================================================================
# Description:
#   Performs a sanity check to ensure the provided path contains valid
#   LLVM headers and CMake configuration files.
#===============================================================================

# Variable: CT_LLVM_INCLUDE_DIR
# Description: Path to LLVM include header files.
set(CT_LLVM_INCLUDE_DIR "${CT_Clang_INSTALL_DIR}/include/llvm")

if (NOT EXISTS "${CT_LLVM_INCLUDE_DIR}")
    message(FATAL_ERROR
            " CT_Clang_INSTALL_DIR (${CT_LLVM_INCLUDE_DIR}) is invalid. "
            "Please ensure it points to the root of the LLVM installation.")
endif (NOT EXISTS "${CT_LLVM_INCLUDE_DIR}")

# Variable: CT_LLVM_CMAKE_FILE
# Description: Path to the specific Clang configuration file.
set(CT_LLVM_CMAKE_FILE
        "${CT_Clang_INSTALL_DIR}/lib/cmake/clang/ClangConfig.cmake")

if (NOT EXISTS "${CT_LLVM_CMAKE_FILE}")
    message(FATAL_ERROR
            " CT_LLVM_CMAKE_FILE (${CT_LLVM_CMAKE_FILE}) is invalid. "
            "Could not locate ClangConfig.cmake.")
endif (NOT EXISTS "${CT_LLVM_CMAKE_FILE}")

#===============================================================================
# SECTION 2: LOAD CLANG CONFIGURATION
# Description:
#   Loads the Clang and LLVM packages. This imports targets like 'clangTooling'
#   and 'LLVMSupport' which are required for linking.
#===============================================================================

# Add local clang cmake directory to search path
list(APPEND CMAKE_PREFIX_PATH "${CT_Clang_INSTALL_DIR}/lib/cmake/clang/")

# Find Package: Clang
# Description: Loads Clang targets and variables. REQUIRED flag halts build on failure.
find_package(Clang REQUIRED CONFIG)

# Version Check
# -------------
# Modern LLVM/Clang API changes are significant. We enforce a minimum version of 16.
if (LLVM_VERSION_MAJOR LESS 16)
    message(FATAL_ERROR "Found LLVM ${LLVM_VERSION_MAJOR}, but need LLVM >= 16. "
                        "Please point CT_Clang_INSTALL_DIR to a newer installation.")
endif ()

# Status Messages
message(STATUS "Found Clang ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using ClangConfig.cmake in: ${CT_Clang_INSTALL_DIR}")
message("CLANG STATUS: 
  Includes (clang)    ${CLANG_INCLUDE_DIRS} 
  Includes (llvm)     ${LLVM_INCLUDE_DIRS}")

# Include Paths
# -------------
# Add LLVM and Clang headers to the system include path to suppress
# warnings arising from external headers.
include_directories(SYSTEM "${LLVM_INCLUDE_DIRS};${CLANG_INCLUDE_DIRS}")

#===============================================================================
# SECTION 3: COMPILER AND BUILD FLAGS
#===============================================================================

# C Standard: C90
set(CMAKE_C_STANDARD 90 CACHE STRING "C Standard Version")

# C++ Standard: C++20
# Description: LLVM 16+ utilizes C++17, but we default to C++20 for
# modern features and forward compatibility.
set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ Standard Version")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Disable GNU extensions (e.g., -std=gnu++20)

# Build Type
# ----------
# Default to Debug if not specified.
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE
            STRING "Build type (default Debug):" FORCE)
endif (NOT CMAKE_BUILD_TYPE)

# Coverage (LLVM/Clang)
# ---------------------
option(TYPE_CORRECT_ENABLE_COVERAGE "Enable LLVM/Clang coverage instrumentation" OFF)
if (TYPE_CORRECT_ENABLE_COVERAGE)
    if (CMAKE_C_COMPILER_ID MATCHES "Clang" OR
        CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate -fcoverage-mapping)
    else ()
        message(FATAL_ERROR "Coverage requires Clang/AppleClang.")
    endif ()
endif ()

# Compiler Flags Interface Library
# --------------------------------
# Defines an interface library to centrally manage compiler flags. Sub-targets
# link against this to inherit flags.
foreach (lang "c" "cxx")
    string(TOUPPER "${lang}" LANG)

    # Target: ${PROJECT_UNDER_NAME}_${lang}_compiler_flags
    add_library("${PROJECT_UNDER_NAME}_${lang}_compiler_flags" INTERFACE)

    # Feature: Require standard version
    target_compile_features(
            "${PROJECT_UNDER_NAME}_${lang}_compiler_flags"
            INTERFACE
            "${lang}_std_${CMAKE_${LANG}_STANDARD}"
    )

    # Generator expressions for compiler identification
    set(gcc_like "$<COMPILE_LANG_AND_ID:C,CXX,ARMClang,AppleClang,Clang,GNU,LCC>")
    set(clang_like "$<COMPILE_LANG_AND_ID:C,CXX,ARMClang,AppleClang,Clang,GNU,LCC>")
    set(msvc "$<COMPILE_LANG_AND_ID:C,CXX,MSVC>")

    # Target Options:
    # 1. GCC/Clang: Enable specific warnings and color diagnostics.
    # 2. Clang-specific: Enable ASan (AddressSanitizer) and debug symbols.
    # 3. MSVC: High warning levels and standards compliance.
    target_compile_options(
            "${PROJECT_UNDER_NAME}_${lang}_compiler_flags"
            INTERFACE
            "$<${gcc_like}:$<BUILD_INTERFACE:-Wall;-Wextra;-Wshadow;-Wformat=2;-Wunused;-fdiagnostics-color=always>>"
            "$<${clang_like}:$<BUILD_INTERFACE:-fno-omit-frame-pointer;-fsanitize=address;-g>>"
            "$<${msvc}:$<BUILD_INTERFACE:-W3;-WX;-Zi;-permissive->>"
    )
endforeach (lang "c" "cxx")

# Linker flags for Darwin/Clang to support Sanitizers
if (
        CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
        CMAKE_C_COMPILER_ID STREQUAL "AppleClang" OR
        CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
        CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -g -fno-omit-frame-pointer -fsanitize=address")
endif ()

# RTTI Configuration
# ------------------
# LLVM is typically built without RTTI (Run-Time Type Information).
# We must disable it in this project to link successfully against LLVM variables.
if(NOT LLVM_ENABLE_RTTI)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

# Output Directories
# ------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

# Configuration Header
# --------------------
# Uses cmake/config.h.in to generate a version header.
configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in"
        "${PROJECT_NAME}Config.h"
)

#===============================================================================
# SECTION 4: ADD SUB-TARGETS
#===============================================================================

# Add Main Source Directory
add_subdirectory("${PROJECT_UNDER_NAME}")

# Add Tests (Optional)
option(BUILD_TESTS "Build tests" ON)
include(CTest)
if (BUILD_TESTING)
    add_subdirectory("${PROJECT_UNDER_NAME}/tests")
endif (BUILD_TESTING)

# Coverage + Doc Coverage Targets
# -------------------------------
if (TYPE_CORRECT_ENABLE_COVERAGE)
    add_custom_target(coverage
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/coverage.sh" "${CMAKE_CURRENT_BINARY_DIR}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            DEPENDS type_correct_cli test_type_correct
            USES_TERMINAL
    )
endif ()

option(TYPE_CORRECT_ENABLE_DOC_COVERAGE "Enable Doxygen doc coverage target" OFF)
if (TYPE_CORRECT_ENABLE_DOC_COVERAGE)
    find_package(Doxygen REQUIRED)
    set(DOCS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/docs")
    configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in"
            "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            @ONLY
    )
    add_custom_target(doc-coverage
            COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            USES_TERMINAL
    )
endif ()

# Packaging and Installation
# --------------------------
include(GNUInstallDirs)

# Install Version Header
install(
        FILES "${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.h"
        TYPE "INCLUDE"
)

include(InstallRequiredSystemLibraries)

# CPack Definitions
set(CPACK_BUNDLE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "SamuelMarks")
set(CPACK_PACKAGE_DESCRIPTION "Solution to 'fix' types, rewriting inconsistent use of types to make them consistent")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")

if (APPLE)
    # Resources for Apple Bundle
    set(CPACK_BUNDLE_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist")
    set(CPACK_BUNDLE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist") # Note: This seems to point to plist in original, retained for consistency
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CustomVolumeIcon.icns")
endif (APPLE)

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md")
set(CPACK_PACKAGE_VERSION_MAJOR "${${PROJECT_NAME}_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${${PROJECT_NAME}_VERSION_MINOR}")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/cmake/README.txt")
set(CPACK_RESOURCE_FILE_WELCOME "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Welcome.txt")
set(CPACK_PACKAGE_CONTACT "https://github.com/SamuelMarks/type-correct")

include(CPack)
include(CMakePackageConfigHelpers)

# Generate Config.cmake
# -------------------
# Allows other projects to find this package via find_package(TypeCorrect).
configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}"
        NO_SET_AND_CHECK_MACRO
        NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

# Generate Version File
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
        VERSION "${${PROJECT_NAME}_VERSION_MAJOR}.${${PROJECT_NAME}_VERSION_MINOR}"
        COMPATIBILITY AnyNewerVersion
)

# Install Config Files
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}")
