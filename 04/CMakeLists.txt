cmake_minimum_required(VERSION 3.15..3.30)

project(opusfun03 VERSION 1.0
                  DESCRIPTION "Fun with compressed audio"
                  LANGUAGES C CXX)

# Override the default in hiredis so we end up with a single executable.
OPTION(BUILD_SHARED_LIBS "Build shared libraries" OFF)
add_subdirectory(hiredis)
add_subdirectory(opus)

# add_executable(play play.cpp WAVFileReader.cpp opus-tools/src/resample.c)
add_executable(play play.cpp opus-tools/src/resample.c)

target_compile_definitions(play PRIVATE OUTSIDE_SPEEX RANDOM_PREFIX=opustools)
target_link_libraries(play hiredis opus)
