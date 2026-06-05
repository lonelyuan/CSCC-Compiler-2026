cmake_minimum_required(VERSION 3.16)
project(ContestantRuntime LANGUAGES C CXX)

add_library(contestant_runtime STATIC runtime_stub.cpp)
target_compile_features(contestant_runtime PRIVATE cxx_std_17)
set_target_properties(contestant_runtime PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
)
