cmake_minimum_required(VERSION 3.16)
project(Compiler2026Submission LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(runtime)
add_subdirectory(pass)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    ${CMAKE_BINARY_DIR}/manifest.json
    COPYONLY
)

