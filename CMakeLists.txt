# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)
project(DanziLoaderCpp VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "DanziLoader-Cpp targets Windows x64 with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_compile_options(/W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zi)
add_link_options(/DEBUG:FULL
  "$<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>"
  "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>"
  "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

set(DANZI_ORACLE_EXE "${CMAKE_SOURCE_DIR}/build/oracle/release/oracle-dirty.exe" CACHE FILEPATH "Rust reference oracle")

add_library(danzi_core STATIC
  src/core/env.cpp
  src/core/watch.cpp
  src/core/inplace.cpp
  src/core/title_copy.cpp
  src/core/shadow.cpp
  src/core/fastmem.cpp
  src/core/wide.cpp
  src/core/hybrid_watch.cpp
  src/core/memcopy.cpp
  src/core/readback.cpp
  src/core/windows.cpp
)
target_compile_options(danzi_core PUBLIC /arch:AVX2)
target_include_directories(danzi_core PUBLIC src)
target_link_libraries(danzi_core PUBLIC ntdll)

add_library(danzi_gpu STATIC
  src/gpu/device.cpp
  src/gpu/card.cpp
  src/gpu/rebar.cpp
  src/gpu/lookup.cpp
  src/gpu/inplace_heap.cpp
)
target_link_libraries(danzi_gpu PUBLIC danzi_core d3d12 dxgi d3dcompiler)

add_executable(dirty-run-cpp tests/dirty/runner.cpp)
target_link_libraries(dirty-run-cpp PRIVATE danzi_core danzi_gpu onecore)

add_executable(dirty-diff tools/dirty_diff.cpp)

add_library(danzi_d3d12x STATIC
  src/d3d12x/object.cpp
  src/d3d12x/device.cpp
  src/d3d12x/dxgi.cpp
)
target_link_libraries(danzi_d3d12x PUBLIC danzi_gpu)
add_library(danzi_backend SHARED src/d3d12x/backend.cpp)
target_link_libraries(danzi_backend PRIVATE danzi_d3d12x)

add_executable(title-driver tools/title_driver.cpp)
target_link_libraries(title-driver PRIVATE dxguid)
add_executable(title-driver-ref tools/title_driver.cpp)
target_link_libraries(title-driver-ref PRIVATE dxguid)
target_compile_definitions(title-driver-ref PRIVATE "DANZI_FIXED_BACKEND=\"${CMAKE_SOURCE_DIR}/build/m4-target/release/victory_ref.dll\"")
add_executable(title-driver-cpp tools/title_driver.cpp)
target_link_libraries(title-driver-cpp PRIVATE dxguid)
target_compile_definitions(title-driver-cpp PRIVATE "DANZI_FIXED_BACKEND=\"$<TARGET_FILE:danzi_backend>\"")
add_dependencies(title-driver-cpp danzi_backend)

add_executable(gpu-bench tools/gpu_bench.cpp)
target_link_libraries(gpu-bench PRIVATE danzi_gpu)

add_executable(danzi-core-tests tests/unit/main.cpp tests/unit/watch_tests.cpp tests/unit/shadow_tests.cpp tests/unit/fastmem_tests.cpp tests/unit/hybrid_tests.cpp tests/unit/gpu_tests.cpp tests/unit/m39_tests.cpp tests/unit/window_tests.cpp)
target_link_libraries(danzi-core-tests PRIVATE danzi_core danzi_gpu onecore)

enable_testing()
add_test(NAME core-unit COMMAND danzi-core-tests)

if(EXISTS "${DANZI_ORACLE_EXE}")
  add_test(NAME dirty-scenarios
    COMMAND dirty-diff suite "${CMAKE_SOURCE_DIR}/tests/dirty/scenarios" "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp>
            "${CMAKE_BINARY_DIR}/dirty-work/suite")
  set_tests_properties(dirty-scenarios PROPERTIES ENVIRONMENT "VICTORY_HYBRID_WATCH=0")
  add_test(NAME dirty-card-budget
    COMMAND dirty-diff --title suite "${CMAKE_SOURCE_DIR}/tests/dirty/budget" "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp>
            "${CMAKE_BINARY_DIR}/dirty-work/budget")
  add_test(NAME dirty-util-fuzz
    COMMAND dirty-diff utilfuzz 1 200 "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp> "${CMAKE_BINARY_DIR}/dirty-work/util-fuzz")
  add_test(NAME dirty-rebar
    COMMAND dirty-diff --title suite "${CMAKE_SOURCE_DIR}/tests/dirty/rebar" "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp>
            "${CMAKE_BINARY_DIR}/dirty-work/rebar")
  add_test(NAME m4-title-suite
    COMMAND dirty-diff suite "${CMAKE_SOURCE_DIR}/tests/m4" $<TARGET_FILE:title-driver-ref> $<TARGET_FILE:title-driver-cpp>
            "${CMAKE_BINARY_DIR}/dirty-work/m4")
  add_test(NAME dirty-hybrid-regressions
    COMMAND dirty-diff --contents suite "${CMAKE_SOURCE_DIR}/tests/dirty/hybrid" "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp>
            "${CMAKE_BINARY_DIR}/dirty-work/hybrid")
  add_test(NAME dirty-hybrid-suite
    COMMAND dirty-diff --contents suite "${CMAKE_SOURCE_DIR}/tests/dirty/scenarios" "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp>
            "${CMAKE_BINARY_DIR}/dirty-work/hybrid-suite")
  set_tests_properties(dirty-hybrid-suite PROPERTIES ENVIRONMENT "VICTORY_HYBRID_WATCH=1;VICTORY_HYBRID_IDLE=1")
  add_test(NAME dirty-fuzz
    COMMAND dirty-diff fuzz 1 200 "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp> "${CMAKE_BINARY_DIR}/dirty-work/fuzz")
  set_tests_properties(dirty-fuzz PROPERTIES ENVIRONMENT "VICTORY_HYBRID_WATCH=0")
  add_test(NAME dirty-fuzz-contents
    COMMAND dirty-diff --contents fuzz 1 200 "${DANZI_ORACLE_EXE}" $<TARGET_FILE:dirty-run-cpp> "${CMAKE_BINARY_DIR}/dirty-work/fuzz-contents")
else()
  message(WARNING "Rust oracle not found at ${DANZI_ORACLE_EXE}: differential tests not registered (run tools/build.cmd).")
endif()
