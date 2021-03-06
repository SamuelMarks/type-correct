###########
# Library #
###########

set(LIBRARY_NAME "${PROJECT_UNDER_NAME}")

set(Header_Files "TypeCorrect.h")
source_group("Header Files" FILES "${Header_Files}")

set(Source_Files "TypeCorrect.cpp")
source_group("Source Files" FILES "${Source_Files}")

add_library("${LIBRARY_NAME}" SHARED "${Header_Files}" "${Source_Files}")
target_include_directories(
        "${LIBRARY_NAME}"
        PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
        "${LIBRARY_NAME}"
        PUBLIC
        "LLVMSupport"
        "clangTooling"
)
target_link_libraries(
        "${LIBRARY_NAME}"
        INTERFACE
        "${PROJECT_UNDER_NAME}_cxx_compiler_flags"
)
if (BUILD_TESTING)
    target_compile_definitions("${LIBRARY_NAME}" PUBLIC "TYPE_CORRECT_DEBUG=1" "TYPE_CORRECT_TEST=1")
endif (BUILD_TESTING)

#######
# CLI #
#######

set(EXEC_NAME "${LIBRARY_NAME}_cli")

set(Cli_Header_Files "TypeCorrectMain.h")
source_group("${EXEC_NAME} Header Files" FILES "${Header_Files}")

set(Cli_Source_Files "TypeCorrectMain.cpp")
source_group("${EXEC_NAME} Source Files" FILES "${Source_Files}")

add_executable("${EXEC_NAME}" "${Cli_Header_Files}" "${Cli_Source_Files}")

target_link_libraries("${EXEC_NAME}" PRIVATE "${LIBRARY_NAME}")
set_target_properties("${LIBRARY_NAME}" PROPERTIES LINKER_LANGUAGE CXX)

include(GenerateExportHeader)
set(_export_file "${CMAKE_CURRENT_BINARY_DIR}/${LIBRARY_NAME}_export.h")
generate_export_header("${LIBRARY_NAME}" EXPORT_FILE_NAME "${_export_file}")

#################
# Install rules #
#################

include(GNUInstallDirs)

# setup the version numbering
set_property(TARGET "${LIBRARY_NAME}" PROPERTY VERSION "1.0.0")
set_property(TARGET "${LIBRARY_NAME}" PROPERTY SOVERSION "1")

set(installable_libs # "${EXEC_NAME}"
        "${LIBRARY_NAME}" "${PROJECT_UNDER_NAME}_cxx_compiler_flags")
if (TARGET "${DEPENDANT_LIBRARY}")
    list(APPEND installable_libs "${DEPENDANT_LIBRARY}")
endif ()
install(TARGETS ${installable_libs}
        EXPORT "${LIBRARY_NAME}Targets"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${_export_file}" ${Header_Files}
        TYPE "INCLUDE")
install(EXPORT "${LIBRARY_NAME}Targets" DESTINATION "${CMAKE_INSTALL_DATADIR}/${LIBRARY_NAME}")

install(FILES "${Header_Files}" "${Cli_Header_Files}" "${_export_file}"
        TYPE "INCLUDE")
