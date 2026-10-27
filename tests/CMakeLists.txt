# Standalone unit tests for the platform-independent core.
# They do not need libobs:  cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.16)
project(tech-tutorial-suite-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(core_dir "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_library(ts-core STATIC "${core_dir}/core/shortcut-filter.cpp" "${core_dir}/core/zoom-logic.cpp")
target_include_directories(ts-core PUBLIC "${core_dir}")
if(MSVC)
  target_compile_options(ts-core PRIVATE /W4 /WX)
else()
  target_compile_options(ts-core PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
foreach(test_name test-shortcut-filter test-zoom-logic)
  add_executable(${test_name} ${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE ts-core)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
