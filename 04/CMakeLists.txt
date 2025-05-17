cmake_minimum_required(VERSION 3.15..3.30)

project(opusfun03 VERSION 1.0
                  DESCRIPTION "Fun with compressed audio"
                  LANGUAGES C CXX)

# Based on https://cliutils.gitlab.io/modern-cmake/chapters/projects/submodule.html
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
# Update submodules as needed
    option(GIT_SUBMODULE "Check submodules during build" ON)
    if(GIT_SUBMODULE)
        message(STATUS "Submodule update")
        execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE GIT_SUBMOD_RESULT)
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update --init --recursive failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
        endif()
    endif()
endif()
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/hiredis/CMakeLists.txt")
    message(FATAL_ERROR "The submodules were not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
endif()

# Override the default in hiredis so we end up with a single executable.
OPTION(BUILD_SHARED_LIBS "Build shared libraries" OFF)
add_subdirectory(hiredis)
add_subdirectory(opus)

add_executable(play play.cpp opus-tools/src/resample.c)

target_compile_definitions(play PRIVATE OUTSIDE_SPEEX RANDOM_PREFIX=opustools)
target_include_directories(play PRIVATE opus-tools/src)
target_link_libraries(play hiredis opus)
