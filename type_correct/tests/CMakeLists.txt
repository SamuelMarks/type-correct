get_filename_component(_LIBRARY_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set(_LIBRARY_NAME "${PROJECT_LOWER_NAME}_${_LIBRARY_NAME}")

set(installable_libs "${PROJECT_LOWER_NAME}_compiler_flags")

set(All_Header_Files "")

set(_built "")
option(BUILD_MATH "Build math tests" OFF)

# Build test libraries
if (BUILD_MATH)
    foreach(suffix "b4" "l8")
        set(LIBRARY_NAME "math_${suffix}")

        set(Header_Files "math_${suffix}.h")
        source_group("${LIBRARY_NAME} Header Files" FILES "${Header_Files}")

        set(Source_Files "math_${suffix}.c")
        source_group("${LIBRARY_NAME} Source Files" FILES "${Source_Files}")

        set(LIBRARY_NAME "math_${suffix}")
        list(APPEND _built "math_${suffix}")

        add_library("${LIBRARY_NAME}" SHARED "${Header_Files}" "${Source_Files}")
        target_include_directories(
                "${LIBRARY_NAME}"
                PUBLIC
                "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
                "$<INSTALL_INTERFACE:include>"
        )
        target_link_libraries("${LIBRARY_NAME}" INTERFACE "${PROJECT_LOWER_NAME}_compiler_flags")
        set_target_properties("${LIBRARY_NAME}" PROPERTIES LINKER_LANGUAGE C)

        include(GenerateExportHeader)
        set(_export_file "${CMAKE_CURRENT_SOURCE_DIR}/${LIBRARY_NAME}_export.h")
        generate_export_header("${LIBRARY_NAME}" EXPORT_FILE_NAME "${_export_file}")

        # setup the version numbering
        set_property(TARGET "${LIBRARY_NAME}" PROPERTY VERSION "1.0.0")
        set_property(TARGET "${LIBRARY_NAME}" PROPERTY SOVERSION "1")

        # install rules prep
        set(installable_libs "${LIBRARY_NAME}")
        list(APPEND All_Header_Files "${Header_Files}")

        # install rules
        if (TARGET "${DEPENDANT_LIBRARY}")
            list(APPEND installable_libs "${DEPENDANT_LIBRARY}")
        endif ()
        install(TARGETS ${installable_libs}
                DESTINATION "lib"
                EXPORT "${LIBRARY_NAME}Targets")

        install(FILES "${Header_Files}" "${_export_file}" DESTINATION "include")
    endforeach ()
endif(BUILD_MATH)

# Get test runner dependency

include(FindGTest)
find_package(GTest QUIET)
if (GTest_FOUND)
    set(gtest_lib "GTest::gtest_main")
else ()
    include("../../cmake/modules/AcquireGoogleTest.cmake")
    acquire_google_test()
    set(gtest_lib "gtest_main")
endif ()

# Build test executables, `add_test` them
foreach (name ${_built} "test_type_correct")
    string(REPLACE "test_" "" _lib "${name}")
    set(_libs "${_lib}" "${gtest_lib}")

    if (NOT (name STREQUAL "test_type_correct"))
        set(name "test_${name}")
    endif ()

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

    set(installable_libs "${EXEC_NAME}")
    if (TARGET "${DEPENDANT_LIBRARY}")
        list(APPEND installable_libs "${DEPENDANT_LIBRARY}")
    endif ()
    install(TARGETS ${installable_libs}
            DESTINATION "bin"
            EXPORT "${EXEC_NAME}Targets")
endforeach()
