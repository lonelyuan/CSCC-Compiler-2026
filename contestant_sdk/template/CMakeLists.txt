cmake_minimum_required(VERSION 3.16)
project(Compiler2026ContestantBaseline LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(runtime)
add_subdirectory(llvm-pass)

# Mirror the manifest into the build tree so either the source tree or build
# tree can be used as the manifest root during local experiments.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    ${CMAKE_BINARY_DIR}/manifest.json
    COPYONLY
)
