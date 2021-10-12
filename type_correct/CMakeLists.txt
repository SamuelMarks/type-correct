get_filename_component(LIBRARY_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set(EXEC_NAME "${LIBRARY_NAME}_cli")

set(Header_Files "TypeCorrect.h")
source_group("Header Files" FILES "${Header_Files}")

set(Source_Files "TypeCorrect.cpp")
source_group("Source Files" FILES "${Source_Files}")

source_group("CLI Source Files" FILES "TypeCorrectMain.cpp")

add_library(
        "${LIBRARY_NAME}"
        SHARED
        "${Header_Files}"
        "${Source_Files}"
)

target_link_libraries("${LIBRARY_NAME}" INTERFACE "${PROJECT_LOWER_NAME}_compiler_flags")

add_executable("${EXEC_NAME}" "TypeCorrectMain.cpp")
target_link_libraries("${EXEC_NAME}" "${LIBRARY_NAME}")

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
install(FILES "${Header_Files}" DESTINATION "include")
