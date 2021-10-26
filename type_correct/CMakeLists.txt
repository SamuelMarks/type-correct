get_filename_component(LIBRARY_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set(EXEC_NAME "${LIBRARY_NAME}_cli")

set(Header_Files "TypeCorrect.h")
source_group("Header Files" FILES "${Header_Files}")

set(Source_Files "TypeCorrect.cpp")
source_group("Source Files" FILES "${Source_Files}")

source_group("CLI Source Files" FILES "TypeCorrectMain.cpp")

add_library("${LIBRARY_NAME}" SHARED "${Header_Files}" "${Source_Files}")
target_include_directories(
        "${LIBRARY_NAME}"
        PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
        "${LIBRARY_NAME}"
        PUBLIC
        "LLVMSupport"
        "clangTooling"
        "${PROJECT_LOWER_NAME}_compiler_flags"
)

set(Cli_Header_Files "TypeCorrectMain.h")
source_group("${EXEC_NAME} Header Files" FILES "${Header_Files}")

set(Cli_Source_Files "TypeCorrectMain.cpp")
source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}")

add_executable("${EXEC_NAME}" "${Cli_Header_Files}" "${Cli_Source_Files}")

target_link_libraries("${EXEC_NAME}" PRIVATE "${LIBRARY_NAME}")
set_target_properties("${LIBRARY_NAME}" PROPERTIES LINKER_LANGUAGE CXX)

# install rules
include(GenerateExportHeader)
set(_export_file "${CMAKE_CURRENT_SOURCE_DIR}/${LIBRARY_NAME}_export.h")
generate_export_header("${LIBRARY_NAME}" EXPORT_FILE_NAME "${_export_file}")

# setup the version numbering
set_property(TARGET "${LIBRARY_NAME}" PROPERTY VERSION "1.0.0")
set_property(TARGET "${LIBRARY_NAME}" PROPERTY SOVERSION "1")

# install rules
set(installable_libs "${EXEC_NAME}" "${LIBRARY_NAME}" "${PROJECT_LOWER_NAME}_compiler_flags")
if (TARGET "${DEPENDANT_LIBRARY}")
    list(APPEND installable_libs "${DEPENDANT_LIBRARY}")
endif ()
install(TARGETS ${installable_libs}
        DESTINATION "bin"
        EXPORT "${LIBRARY_NAME}Targets")
install(FILES "${Header_Files}" "${Cli_Header_Files}" "${_export_file}" DESTINATION "include")
