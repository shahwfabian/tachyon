cmake_minimum_required(VERSION 3.20)
project(tachyon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TACHYON_NATIVE "Tune for the build machine (-march=native)" ON)
option(TACHYON_SANITIZE "Build tests and fuzzer with ASan + UBSan" OFF)

add_library(tachyon INTERFACE)
target_include_directories(tachyon INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(tachyon INTERFACE -Wall -Wextra -Wpedantic)
if(TACHYON_NATIVE)
  target_compile_options(tachyon INTERFACE -march=native)
endif()

add_executable(tachyon_tests tests/tests.cpp)
target_link_libraries(tachyon_tests PRIVATE tachyon)

add_executable(tachyon_fuzz fuzz/fuzz.cpp)
target_link_libraries(tachyon_fuzz PRIVATE tachyon)
target_compile_definitions(tachyon_fuzz PRIVATE TACHYON_PLANTED_BUGS)

add_executable(tachyon_bench bench/bench.cpp)
target_link_libraries(tachyon_bench PRIVATE tachyon)

if(TACHYON_SANITIZE)
  foreach(t tachyon_tests tachyon_fuzz)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

enable_testing()
add_test(NAME unit COMMAND tachyon_tests)
add_test(NAME fuzz_smoke COMMAND tachyon_fuzz --clean 500 --out fuzz_smoke.json)
