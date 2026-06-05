cmake_minimum_required(VERSION 3.16)
project(ContestantRuntime LANGUAGES C CXX)

add_library(contestant_runtime STATIC dag_runtime.cpp)
target_compile_features(contestant_runtime PRIVATE cxx_std_17)
target_compile_options(contestant_runtime PRIVATE -fPIC)
set_target_properties(contestant_runtime PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
)

