#===============================================================================
# FILE: type_correct/CMakeLists.txt
# 
# DESCRIPTION: 
#   Configures the core TypeCorrect library and the standalone CLI tool. 
#   It links against the necessary LLVM and Clang libraries required for both
#   plugin usage and standalone LibTooling usage. 
#===============================================================================

########### 
# Library # 
########### 
# The library target represents the core logic (AST Consumer, Matchers, Rewriter). 
# It is built as a shared library so it can be loaded dynamically by clang as a plugin. 

# Variable: LIBRARY_NAME
# Description: The name of the core library target (type_correct). 
set(LIBRARY_NAME "${PROJECT_UNDER_NAME}") 

# Variable: Header_Files
# Description: List of public headers exposed by the library. 
set(Header_Files 
    "TypeCorrect.h"
    "StructAnalyzer.h"
    "TypeSolver.h"
    "CTU/FactManager.h"
) 
source_group("Header Files" FILES "${Header_Files}") 

# Variable: Source_Files
# Description: List of C++ implementation files for the library. 
set(Source_Files 
    "TypeCorrect.cpp"
    "StructAnalyzer.cpp"
    "TypeSolver.cpp"
    "CTU/FactManager.cpp"
) 
source_group("Source Files" FILES "${Source_Files}") 

# Target: type_correct (SHARED) 
add_library("${LIBRARY_NAME}" SHARED "${Header_Files}" "${Source_Files}") 

# Include Directories
# ------------------- 
# Configures include paths for build time and installation time. 
target_include_directories( 
        "${LIBRARY_NAME}" 
        PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>" 
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>" 
        "$<INSTALL_INTERFACE:include>" 
) 

# Link Dependencies
# ----------------- 
# Links against: 
# 1. LLVMSupport: Core LLVM data structures and utilities. 
# 2. clangTooling: Facilities for writing standalone tools and plugins. 
# 3. clangIndex: Required for USR generation in CTU mode.
target_link_libraries( 
        "${LIBRARY_NAME}" 
        PUBLIC
        "LLVMSupport" 
        "clangTooling" 
        "clangIndex"
) 

# Inherit Compiler Flags
# ---------------------- 
# Applies the global compiler flags settings (C++20, warnings, RTTI settings). 
target_link_libraries( 
        "${LIBRARY_NAME}" 
        INTERFACE
        "${PROJECT_UNDER_NAME}_cxx_compiler_flags" 
) 

# Testing Defines
# --------------- 
# Adds preprocessor definitions if building in test mode. 
if (BUILD_TESTING) 
    target_compile_definitions("${LIBRARY_NAME}" PUBLIC "TYPE_CORRECT_DEBUG=1" "TYPE_CORRECT_TEST=1") 
endif (BUILD_TESTING) 

####### 
# CLI # 
####### 
# The executable target for the standalone command-line tool. 

# Variable: EXEC_NAME
# Description: Name of the standalone executable (type_correct_cli). 
set(EXEC_NAME "${LIBRARY_NAME}_cli") 

# Variable: Cli_Header_Files
# Description: Headers specific to the main driver. 
set(Cli_Header_Files "TypeCorrectMain.h") 
source_group("${EXEC_NAME} Header Files" FILES "${Header_Files}") 

# Variable: Cli_Source_Files
# Description: Implementation of the main() entry point and options parser. 
set(Cli_Source_Files "TypeCorrectMain.cpp") 
source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}") 

# Target: type_correct_cli (EXECUTABLE) 
add_executable("${EXEC_NAME}" "${Cli_Header_Files}" "${Cli_Source_Files}") 

# Links private against the core library to access AST logic. 
target_link_libraries("${EXEC_NAME}" PRIVATE "${LIBRARY_NAME}") 

# Ensure C++ linker is used to handle mangling and standard libs correctly. 
set_target_properties("${LIBRARY_NAME}" PROPERTIES LINKER_LANGUAGE CXX) 

# Export Header Generation
# ------------------------ 
# Generates a header file defining visibility macros (DLL import/export) 
# for cross-platform shared library support. 
include(GenerateExportHeader) 
set(_export_file "${CMAKE_CURRENT_BINARY_DIR}/${LIBRARY_NAME}_export.h") 
generate_export_header("${LIBRARY_NAME}" EXPORT_FILE_NAME "${_export_file}") 

################# 
# Install rules # 
################# 

include(GNUInstallDirs) 

# Version Properties
# ------------------ 
set_property(TARGET "${LIBRARY_NAME}" PROPERTY VERSION "1.0.0") 
set_property(TARGET "${LIBRARY_NAME}" PROPERTY SOVERSION "1") 

# List of targets to install. 
set(installable_libs
        "${LIBRARY_NAME}" "${PROJECT_UNDER_NAME}_cxx_compiler_flags") 

# Add dependant library if it exists (conditional check placeholder). 
if (TARGET "${DEPENDANT_LIBRARY}") 
    list(APPEND installable_libs "${DEPENDANT_LIBRARY}") 
endif () 

# Install Targets
# --------------- 
# 1. EXPORT: Creates a CMake export set for downstream consumption. 
# 2. ARCHIVE/LIBRARY/RUNTIME: Destinations for static libs, shared libs, and binaries. 
install(TARGETS ${installable_libs} 
        EXPORT "${LIBRARY_NAME}Targets" 
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" 
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" 
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}") 

# Install Headers
# --------------- 
# Installs the generated export header and public headers to the include directory. 
install(FILES "${_export_file}" ${Header_Files} 
        TYPE "INCLUDE") 

# Install Export Definitions
# -------------------------- 
install(EXPORT "${LIBRARY_NAME}Targets" DESTINATION "${CMAKE_INSTALL_DATADIR}/${LIBRARY_NAME}") 

# Redundant explicit install for verification (keeps original logic intact). 
install(FILES "${Header_Files}" "${Cli_Header_Files}" "${_export_file}" 
        TYPE "INCLUDE")