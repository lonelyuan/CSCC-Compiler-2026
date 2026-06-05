cmake_minimum_required(VERSION 3.16)
project(ContestantPass LANGUAGES C CXX)

find_program(LLVM_CONFIG NAMES llvm-config llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17 REQUIRED)

function(run_llvm_config out_var)
  execute_process(
    COMMAND "${LLVM_CONFIG}" ${ARGN}
    RESULT_VARIABLE cmd_status
    OUTPUT_VARIABLE cmd_output
    ERROR_VARIABLE cmd_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT cmd_status EQUAL 0)
    message(FATAL_ERROR "Failed to query llvm-config ${ARGN}: ${cmd_error}")
  endif()
  set(${out_var} "${cmd_output}" PARENT_SCOPE)
endfunction()

run_llvm_config(LLVM_INCLUDE_DIR --includedir)
run_llvm_config(LLVM_CXXFLAGS --cxxflags)
run_llvm_config(LLVM_LDFLAGS --ldflags)
run_llvm_config(LLVM_SHARED_MODE --shared-mode)
run_llvm_config(LLVM_LIBS --libs core passes support)
run_llvm_config(LLVM_SYSTEM_LIBS --system-libs)

separate_arguments(LLVM_CXXFLAGS_LIST UNIX_COMMAND "${LLVM_CXXFLAGS}")
separate_arguments(LLVM_LDFLAGS_LIST UNIX_COMMAND "${LLVM_LDFLAGS}")
separate_arguments(LLVM_LIBS_LIST UNIX_COMMAND "${LLVM_LIBS}")
separate_arguments(LLVM_SYSTEM_LIBS_LIST UNIX_COMMAND "${LLVM_SYSTEM_LIBS}")

list(FILTER LLVM_CXXFLAGS_LIST EXCLUDE REGEX "^-std=")
list(FILTER LLVM_CXXFLAGS_LIST EXCLUDE REGEX "^-I")

add_library(contestant_pass MODULE dag_pass.cpp)
target_compile_features(contestant_pass PRIVATE cxx_std_17)
target_include_directories(contestant_pass SYSTEM PRIVATE "${LLVM_INCLUDE_DIR}")
target_compile_options(contestant_pass PRIVATE ${LLVM_CXXFLAGS_LIST})
target_link_options(contestant_pass PRIVATE ${LLVM_LDFLAGS_LIST})

if(NOT LLVM_SHARED_MODE STREQUAL "static")
  target_link_libraries(contestant_pass PRIVATE ${LLVM_LIBS_LIST} ${LLVM_SYSTEM_LIBS_LIST})
endif()

set_target_properties(contestant_pass PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pass"
)

