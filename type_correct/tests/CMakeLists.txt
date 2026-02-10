# Get test runner dependency
include(FindGTest) 

# Request both gtest_main (for the main() entry point) and gmock (for Matchers)
find_package(GTest QUIET COMPONENTS gtest_main gmock) 

if (GTest_FOUND) 
    # Standard CMake package targets
    set(test_libs GTest::gtest_main GTest::gmock) 
else () 
    # Fallback to fetching from source
    include("${CMAKE_SOURCE_DIR}/cmake/modules/AcquireGoogleTest.cmake") 
    acquire_google_test() 
    # FetchContent defines these target names
    set(test_libs gtest_main gmock) 
endif () 

option(BUILD_MATH_TYPE "Build math_type tests" ON) 

# Build lower-level test libraries
if (BUILD_MATH_TYPE) 
    add_subdirectory("math_type") 
endif (BUILD_MATH_TYPE) 

################## 
# Top-level test # 
################## 

set(EXEC_NAME "test_type_correct") 

set(Source_Files
    "${EXEC_NAME}.cpp"
    "test_coverage.cpp"
) 
source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}") 

add_executable("${EXEC_NAME}" "${Source_Files}") 
set_target_properties( 
        "${EXEC_NAME}" 
        PROPERTIES
        LINKER_LANGUAGE
        CXX
) 

# Link against the definition from above (includes GMock now)
target_link_libraries("${EXEC_NAME}" PUBLIC ${test_libs} "${PROJECT_UNDER_NAME}") 

target_compile_definitions("${EXEC_NAME}" PRIVATE
        TYPE_CORRECT_CLI_PATH="$<TARGET_FILE:type_correct_cli>"
        TYPE_CORRECT_BUILD_DIR="$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>"
        TYPE_CORRECT_SOURCE_DIR="$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
)

add_test(NAME "${EXEC_NAME}" COMMAND "${EXEC_NAME}")
