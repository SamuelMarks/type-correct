get_filename_component(_LIBRARY_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set(_LIBRARY_NAME "${PROJECT_LOWER_NAME}_${_LIBRARY_NAME}")

# Get test runner dependency
include(FindGTest)
find_package(GTest QUIET COMPONENTS "gtest_main")
if (GTest_FOUND)
    set(gtest_lib "GTest::gtest_main")
else ()
    include("${CMAKE_SOURCE_DIR}/cmake/modules/AcquireGoogleTest.cmake")
    acquire_google_test()
    set(gtest_lib "gtest_main")
endif ()

option(BUILD_MATH_TYPE "Build math_type tests" ON)

# Build test libraries
if (BUILD_MATH_TYPE)
    add_subdirectory("math_type")
endif (BUILD_MATH_TYPE)

# include(GoogleTest)
# Build test executables, `add_test` them
foreach (name ${_built} "test_type_correct")
    string(REPLACE "test_" "" _lib "${name}")
    set(_libs "${_lib}" "${gtest_lib}")

    if (NOT (name STREQUAL "test_type_correct"))
        set(name "test_${name}")
    endif (NOT (name STREQUAL "test_type_correct"))

    set(EXEC_NAME "${name}")

    set(Source_Files "${name}.cpp")
    source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}")

    add_executable("${EXEC_NAME}" "${Source_Files}")
    set_target_properties(
            "${EXEC_NAME}"
            PROPERTIES
            LINKER_LANGUAGE
            CXX
    )
    message("target_link_libraries\(\"${EXEC_NAME}\" PUBLIC \"${_libs}\"\)")
    target_link_libraries("${EXEC_NAME}" PUBLIC "${_libs}")

    add_test(NAME "${EXEC_NAME}" COMMAND "${EXEC_NAME}")
    # gtest_discover_tests("${EXEC_NAME}")
endforeach (name ${_built} "test_type_correct")
