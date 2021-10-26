get_filename_component(_LIBRARY_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set(_LIBRARY_NAME "${PROJECT_LOWER_NAME}_${_LIBRARY_NAME}")

# JSON file needed to run the type_correct tool
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(installable_libs "${PROJECT_LOWER_NAME}_compiler_flags")

set(All_Header_Files "")

foreach(suffix "b4" "l8")
    set(Header_Files "math_${suffix}.h")
    source_group("Header Files" FILES "${Header_Files}")

    set(Source_Files "math_${suffix}.c")
    source_group("Source Files" FILES "${Source_Files}")

    set(LIBRARY_NAME "${_LIBRARY_NAME}_${suffix}")

    add_library("${LIBRARY_NAME}" SHARED "${Header_Files}" "${Source_Files}")

    target_link_libraries("${LIBRARY_NAME}" INTERFACE "${PROJECT_LOWER_NAME}_compiler_flags")
    set_target_properties("${LIBRARY_NAME}" PROPERTIES LINKER_LANGUAGE C)

    # setup the version numbering
    set_property(TARGET "${LIBRARY_NAME}" PROPERTY VERSION "1.0.0")
    set_property(TARGET "${LIBRARY_NAME}" PROPERTY SOVERSION "1")

    # install rules prep
    list(APPEND installable_libs "${LIBRARY_NAME}")
    list(APPEND All_Header_Files "${Header_Files}")
endforeach ()

include(GenerateExportHeader)
# Can't use `_LIBRARY_NAME` as that still generated `TYPECORRECT_TESTS_L8_EXPORT` &etc
set(_export_file "${CMAKE_CURRENT_SOURCE_DIR}/${LIBRARY_NAME}_export.h")
generate_export_header("${LIBRARY_NAME}" EXPORT_FILE_NAME "${_export_file}")

# Test runner

include(FetchContent)
FetchContent_Declare(
        googletest
        # Specify the commit you depend on and update it regularly.
        URL https://github.com/google/googletest/archive/609281088cfefc76f9d0ce82e1ff6c30cc3591e5.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(EXEC_NAME "test_${PROJECT_LOWER_NAME}")

set(Source_Files "test_math.cpp")
source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}")

set(Header_Files "tests.h")
source_group("Header Files" FILES "${Header_Files}")

add_executable("${EXEC_NAME}" "${Header_Files}" "${Source_Files}")
set_target_properties(
        "${EXEC_NAME}"
        PROPERTIES
        LINKER_LANGUAGE
        CXX
)
target_link_libraries("${EXEC_NAME}" PUBLIC "type_correct" "gtest_main")

add_test(NAME "${EXEC_NAME}" COMMAND "${EXEC_NAME}")

list(APPEND installable_libs "${EXEC_NAME}")

##

# install rules

if (TARGET "${DEPENDANT_LIBRARY}")
    list(APPEND installable_libs "${DEPENDANT_LIBRARY}")
endif ()
install(TARGETS ${installable_libs}
        DESTINATION "lib"
        EXPORT "${_LIBRARY_NAME}Targets")
install(FILES "${All_Header_Files}" "${_export_file}" DESTINATION "include")
