macro (math_type_cmake_lists)
    get_filename_component(suffix "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    set(LIBRARY_NAME "${PROJECT_LOWER_NAME}_math_${suffix}")

    set(Header_Files "math_type.h")
    source_group("Header Files" FILES "${Header_Files}")

    set(Source_Files "math_type.c")
    source_group("Source Files" FILES "${Source_Files}")

    add_library("${LIBRARY_NAME}" "${Header_Files}" "${Source_Files}")
    target_include_directories(
            "${LIBRARY_NAME}"
            PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
            "$<INSTALL_INTERFACE:include>"
    )
    target_link_libraries("${LIBRARY_NAME}" PRIVATE "${PROJECT_LOWER_NAME}_compiler_flags")

    set_target_properties("${LIBRARY_NAME}" PROPERTIES LINKER_LANGUAGE C)

    # setup the version numbering
    set_property(TARGET "${LIBRARY_NAME}" PROPERTY VERSION "1.0.0")
    set_property(TARGET "${LIBRARY_NAME}" PROPERTY SOVERSION "1")

    include(GenerateExportHeader)
    set(_export_file "${CMAKE_CURRENT_BINARY_DIR}/math_type_export.h")
    generate_export_header(
            "${LIBRARY_NAME}"
            BASE_NAME "MATH_TYPE"
            EXPORT_FILE_NAME "${_export_file}"
    )
    set(EXEC_NAME "test_math_type_${suffix}")

    set(Source_Files "${EXEC_NAME}.cpp")
    source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}")

    add_executable("${EXEC_NAME}" "${Source_Files}")
    set_target_properties(
            "${EXEC_NAME}"
            PROPERTIES
            LINKER_LANGUAGE
            CXX
    )
    target_link_libraries("${EXEC_NAME}" PUBLIC "${gtest_lib}")
    target_link_libraries("${EXEC_NAME}" PRIVATE "${LIBRARY_NAME}")

    add_test(NAME "${EXEC_NAME}" COMMAND "${EXEC_NAME}")
endmacro (math_type_cmake_lists)

foreach(suffix "b4" "l8")
    add_subdirectory("${suffix}")
endforeach (suffix "b4" "l8")
