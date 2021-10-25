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
if(APPLE AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    set(CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS "${CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS} -undefined dynamic_lookup")
endif()

#add_executable("${EXEC_NAME}" "TypeCorrectMain.cpp")
#target_link_libraries("${EXEC_NAME}" "${LIBRARY_NAME}")

set(LLVM_LINK_COMPONENTS support)
include("${CMAKE_SOURCE_DIR}/cmake/AddClang.cmake")
add_clang_executable("${EXEC_NAME}"
        "TypeCorrectMain.cpp"
        )
target_link_libraries("${EXEC_NAME}"
        PRIVATE
        "${LIBRARY_NAME}"
        clangAST
        clangASTMatchers
        clangBasic
        clangFrontend
        clangSerialization
        clangTooling
        )

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories("${LLVM_INCLUDE_DIRS}")
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
message(STATUS "LLVM_DEFINITIONS_LIST = ${LLVM_DEFINITIONS_LIST}")
add_definitions(${LLVM_DEFINITIONS_LIST})

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs support core irreader)
message(STATUS "llvm_libs = ${llvm_libs}")
# Link against LLVM libraries
target_link_libraries("${LIBRARY_NAME}" PUBLIC "${llvm_libs}")



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
