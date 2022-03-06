foreach (suffix "b4" "l8")
    set(name "math_type")
    set(LIBRARY_NAME "${PROJECT_UNDER_NAME}_${name}_${suffix}")

    set(Header_Files "${suffix}/${name}.h")
    source_group("Header Files" FILES "${Header_Files}")

    set(Source_Files "${suffix}/${name}.c")
    source_group("Source Files" FILES "${Source_Files}")

    add_library("${LIBRARY_NAME}" "${Header_Files}" "${Source_Files}")
    target_include_directories(
            "${LIBRARY_NAME}"
            PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
            "$<INSTALL_INTERFACE:include>"
    )
    #target_link_libraries("${LIBRARY_NAME}" PRIVATE "${PROJECT_UNDER_NAME}_c_compiler_flags")

    set_target_properties("${LIBRARY_NAME}" PROPERTIES LINKER_LANGUAGE C)

    # setup the version numbering
    set_property(TARGET "${LIBRARY_NAME}" PROPERTY VERSION "1.0.0")
    set_property(TARGET "${LIBRARY_NAME}" PROPERTY SOVERSION "1")

    include(GenerateExportHeader)
    set(_export_file "${CMAKE_CURRENT_BINARY_DIR}/${name}_export.h")
    generate_export_header(
            "${LIBRARY_NAME}"
            BASE_NAME "MATH_TYPE"
            EXPORT_FILE_NAME "${_export_file}"
    )
    set(EXEC_NAME "test_${name}_${suffix}")

    set(Source_Files "${suffix}/${EXEC_NAME}.cpp")
    source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}")

    add_executable("${EXEC_NAME}" "${Source_Files}")
    set_target_properties(
            "${EXEC_NAME}"
            PROPERTIES
            LINKER_LANGUAGE
            CXX
    )
    target_link_libraries("${EXEC_NAME}" PUBLIC "${gtest_lib}")
    target_link_libraries(
            "${EXEC_NAME}"
            PRIVATE
            "${LIBRARY_NAME}"
            "${PROJECT_UNDER_NAME}_cxx_compiler_flags"
    )

    add_test(NAME "${EXEC_NAME}" COMMAND "${EXEC_NAME}")
endforeach (suffix "b4" "l8")
