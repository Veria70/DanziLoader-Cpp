# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)
project(DanziLoaderCpp VERSION 0.1.0 LANGUAGES C CXX)

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
  src/core/policy.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/toml.cpp
  src/core/rust_text.cpp
  src/core/kbm.cpp
)
target_compile_definitions(danzi_core PRIVATE DANZI_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
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

add_library(danzi_formats STATIC src/formats/texture.cpp src/formats/bcpack.cpp)
target_include_directories(danzi_formats PUBLIC src)
add_executable(layout-check tools/layout_check.cpp src/d3d12x/sampler.cpp)
target_link_libraries(layout-check PRIVATE danzi_formats)
add_library(blake3 STATIC
  third_party/blake3/blake3.c
  third_party/blake3/blake3_dispatch.c
  third_party/blake3/blake3_portable.c
  third_party/blake3/blake3_sse2.c
  third_party/blake3/blake3_sse41.c
  third_party/blake3/blake3_avx2.c
  third_party/blake3/blake3_avx512.c)
target_include_directories(blake3 PUBLIC third_party/blake3)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
add_library(danzi_shader STATIC
  src/shader/native.cpp
  src/shader/dxbc.cpp
  src/shader/gfx10.cpp
  src/shader/ir.cpp
  src/shader/spill.cpp
  src/shader/structure.cpp
  src/shader/resource.cpp
  src/shader/io.cpp
  src/shader/bitcode.cpp
  src/shader/bitcode_opt.cpp
  src/shader/container.cpp
  src/shader/dxil.cpp
  src/shader/wave.cpp
  src/shader/own_bits.cpp
  src/shader/stand_in.cpp
  src/shader/lower_a.cpp
  src/shader/lower_b.cpp
  src/shader/lower_c.cpp
  src/shader/lower_d.cpp
  src/shader/lower_e.cpp
  src/shader/lower_f.cpp
  src/shader/lower_g.cpp
  src/shader/lower_h.cpp
  src/shader/lower_i.cpp
  src/shader/lower_j.cpp
  src/shader/lower_k.cpp
  src/shader/lower_l.cpp
  src/shader/lower_m.cpp
  src/shader/lower_n.cpp
  src/shader/emit.cpp
)
target_include_directories(danzi_shader PUBLIC src)
target_link_libraries(danzi_shader PUBLIC blake3)
add_executable(shader-check tools/shader_check.cpp)
target_link_libraries(shader-check PRIVATE danzi_shader dbghelp)

add_library(danzi_d3d12x STATIC
  src/d3d12x/object.cpp
  src/d3d12x/device.cpp
  src/d3d12x/dxgi.cpp
  src/d3d12x/fence.cpp
  src/d3d12x/queue.cpp
  src/d3d12x/frame.cpp
  src/d3d12x/resource.cpp
  src/d3d12x/descriptor.cpp
  src/d3d12x/sampler.cpp
  src/d3d12x/root.cpp
  src/d3d12x/host.cpp
  src/d3d12x/list.cpp
  src/d3d12x/texture.cpp
  src/d3d12x/shader_cache.cpp
  src/d3d12x/host_heap.cpp
  src/d3d12x/pipeline.cpp
  src/d3d12x/draw.cpp
  src/d3d12x/indirect.cpp
  src/d3d12x/heap_pass.cpp
  src/d3d12x/memory.cpp
  src/d3d12x/present.cpp
)
target_link_libraries(danzi_d3d12x PUBLIC danzi_gpu danzi_formats danzi_shader blake3 dxgi d3dcompiler)
add_library(danzi_backend SHARED src/d3d12x/backend.cpp)
target_link_libraries(danzi_backend PRIVATE danzi_d3d12x)

add_executable(title-driver tools/title_driver.cpp)
target_link_libraries(title-driver PRIVATE dxguid dbghelp winmm dwmapi)
add_executable(title-driver-ref tools/title_driver.cpp)
target_link_libraries(title-driver-ref PRIVATE dxguid dbghelp winmm dwmapi)
target_compile_definitions(title-driver-ref PRIVATE "DANZI_FIXED_BACKEND=\"${CMAKE_SOURCE_DIR}/build/m4-target/release/victory_ref.dll\"")
add_executable(title-driver-cpp tools/title_driver.cpp)
target_link_libraries(title-driver-cpp PRIVATE dxguid dbghelp winmm dwmapi)
target_compile_definitions(title-driver-cpp PRIVATE "DANZI_FIXED_BACKEND=\"$<TARGET_FILE:danzi_backend>\"")
add_dependencies(title-driver-cpp danzi_backend)

add_library(danzi_pe STATIC src/pe/image.cpp src/pe/implib.cpp)
target_include_directories(danzi_pe PUBLIC src)
add_library(danzi_loader STATIC src/loader/prepare.cpp src/loader/windows.cpp src/loader/threads.cpp src/loader/tls.cpp
  src/loader/hooks.cpp src/loader/crash.cpp src/loader/emulate.cpp)
target_link_libraries(danzi_loader PUBLIC danzi_pe danzi_core blake3)
add_executable(emulate-check tools/emulate_check.cpp)
target_link_libraries(emulate-check PRIVATE danzi_loader)
add_test(NAME m6-emulate COMMAND emulate-check)
set(M6_FIXTURE_DIR "${CMAKE_BINARY_DIR}/m6-fixture")
add_library(fixture_host SHARED tests/m6/fixture/host.cpp)
add_library(fixture_dll SHARED tests/m6/fixture/dll.cpp tests/m6/fixture/dll.def)
add_executable(fixture_title tests/m6/fixture/title.cpp)
foreach(t fixture_host fixture_dll fixture_title)
  set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${M6_FIXTURE_DIR}" MSVC_RUNTIME_LIBRARY MultiThreaded)
  target_compile_options(${t} PRIVATE /GS-)
endforeach()
target_link_libraries(fixture_dll PRIVATE fixture_host)
target_link_options(fixture_dll PRIVATE /ENTRY:fixture_dll_main)
target_link_libraries(fixture_title PRIVATE fixture_host fixture_dll)
target_link_options(fixture_title PRIVATE /ENTRY:title_entry /SUBSYSTEM:CONSOLE)
add_executable(loader-run tools/loader_run.cpp)
target_link_libraries(loader-run PRIVATE danzi_loader)
add_dependencies(loader-run fixture_title)
add_library(danzi_hle STATIC src/hle/registry.cpp src/hle/table.cpp src/hle/runtime.cpp src/hle/kernel.cpp
  src/hle/xgameruntime.cpp src/hle/areas.cpp src/hle/xasync.cpp src/hle/xuser.cpp src/hle/xsystem.cpp src/hle/xgame.cpp
  src/hle/areas_small.cpp src/hle/xgamesave.cpp src/hle/xstore.cpp src/hle/xgameui.cpp src/hle/files.cpp src/hle/lifecycle.cpp
  src/hle/gameinput_cursor.cpp src/hle/gameinput.cpp src/hle/xmem.cpp src/hle/d3d12x.cpp src/hle/dstorage.cpp src/hle/net.cpp src/hle/media.cpp
  src/hle/audio.cpp src/hle/audio_capture.cpp src/hle/apu.cpp src/hle/dsp.cpp src/hle/xg.cpp src/formats/xma.cpp)
target_link_libraries(danzi_hle PUBLIC danzi_loader danzi_d3d12x opus)
add_executable(hle-run tools/hle_run.cpp)
add_executable(bcpack-check tools/bcpack_check.cpp)
target_link_libraries(bcpack-check PRIVATE danzi_formats)
add_executable(opus-check tools/opus_check.cpp)
target_link_libraries(opus-check PRIVATE opus)
add_executable(toml-check tools/toml_check.cpp)
target_link_libraries(toml-check PRIVATE danzi_core)
add_library(danzi_launcher SHARED apps/victory/main.cpp apps/victory/shader.cpp apps/victory/translate.cpp apps/victory/precompile.cpp
  src/formats/sgd5.cpp src/shader/sp3.cpp src/shader/host.cpp src/shader/validator.cpp)
target_link_libraries(danzi_launcher PRIVATE danzi_hle danzi_core blake3)
target_include_directories(danzi_launcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_executable(victory apps/victory/entry.cpp)
target_link_libraries(victory PRIVATE danzi_launcher)
target_link_libraries(hle-run PRIVATE danzi_hle)
add_executable(pe-check tools/pe_check.cpp)
target_link_libraries(pe-check PRIVATE danzi_pe danzi_loader danzi_core danzi_hle)

add_executable(gpu-bench tools/gpu_bench.cpp)
target_link_libraries(gpu-bench PRIVATE danzi_gpu)

add_executable(danzi-core-tests tests/unit/main.cpp tests/unit/watch_tests.cpp tests/unit/shadow_tests.cpp tests/unit/fastmem_tests.cpp tests/unit/hybrid_tests.cpp tests/unit/gpu_tests.cpp tests/unit/m39_tests.cpp tests/unit/window_tests.cpp tests/unit/shader_tests.cpp tests/unit/kbm_tests.cpp)
target_link_libraries(danzi-core-tests PRIVATE danzi_core danzi_gpu danzi_shader onecore)

enable_testing()
add_test(NAME m4-sweep
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_sweep.py" $<TARGET_FILE:title-driver-cpp> "${CMAKE_BINARY_DIR}/m4-sweep")
add_test(NAME m4-library-trim
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_library_trim.py" $<TARGET_FILE:title-driver-cpp> "${CMAKE_BINARY_DIR}/m4-library-trim")
add_test(NAME m4-pipeline-cache
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_pipeline_cache.py" $<TARGET_FILE:title-driver-cpp> "${CMAKE_BINARY_DIR}/m4-pipeline-cache")
add_test(NAME core-unit COMMAND danzi-core-tests)
add_executable(kbm-check tools/kbm_check.cpp)
target_link_libraries(kbm-check PRIVATE danzi_core)
set(DANZI_KBM_ORACLE "${CMAKE_SOURCE_DIR}/build/kbm-target/release/kbm-oracle.exe")
if(EXISTS "${DANZI_KBM_ORACLE}")
  add_test(NAME m6-kbm
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_kbm.py" "${DANZI_KBM_ORACLE}" $<TARGET_FILE:kbm-check> "${CMAKE_BINARY_DIR}/m6-kbm")
endif()

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
  add_test(NAME m4-heap-index
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_heap_index.py" $<TARGET_FILE:title-driver-ref> $<TARGET_FILE:title-driver-cpp>
            "${CMAKE_SOURCE_DIR}/tests/m4/17-heap-index.txt" "${CMAKE_BINARY_DIR}/m4-heap-index")
  add_test(NAME m4-memory
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_memory.py" $<TARGET_FILE:title-driver-ref> $<TARGET_FILE:title-driver-cpp>
            "${CMAKE_SOURCE_DIR}/tests/m4/19-memory.txt" "${CMAKE_BINARY_DIR}/m4-memory")
  add_test(NAME m5-own-translations
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_own_translations.py" $<TARGET_FILE:title-driver-ref> $<TARGET_FILE:title-driver-cpp>
            "${CMAKE_SOURCE_DIR}/tests/m4" "${CMAKE_BINARY_DIR}/m5-own-translations")
  add_test(NAME m6-pe-fixtures
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_pe.py" "${CMAKE_SOURCE_DIR}/build/m4-target/release/m6-oracle.exe" $<TARGET_FILE:pe-check>
            "${CMAKE_BINARY_DIR}/m6-pe")
  add_test(NAME m6-loader
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_loader.py" "${CMAKE_SOURCE_DIR}/build/m4-target/release/m6-oracle.exe" $<TARGET_FILE:loader-run>
            "${CMAKE_BINARY_DIR}/m6-fixture" "${CMAKE_BINARY_DIR}/m6-loader")
  if(EXISTS "${CMAKE_SOURCE_DIR}/build/m4-target/release/victory.exe")
    add_test(NAME m7-cli
      COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_cli.py" "${CMAKE_SOURCE_DIR}/build/m4-target/release/victory.exe" $<TARGET_FILE:victory>
              "${CMAKE_BINARY_DIR}/m6-fixture" "${CMAKE_BINARY_DIR}/m7-cli")
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/build/toml-target/release/toml-oracle.exe")
    add_test(NAME m7-toml
      COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_toml.py" run --seed 11 --generated 2000 --mutated 4000 --focused 2000
              --port $<TARGET_FILE:toml-check> --work "${CMAKE_BINARY_DIR}/m7-toml")
    add_test(NAME m7-config
      COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_toml.py" run-config --seed 11 --generated 2000 --mutated 4000
              --port $<TARGET_FILE:toml-check> --work "${CMAKE_BINARY_DIR}/m7-config")
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/build/opus-target/release/opus-oracle.exe")
    add_test(NAME m6-opus
      COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_opus.py" "${CMAKE_SOURCE_DIR}/build/opus-target/release/opus-oracle.exe" $<TARGET_FILE:opus-check>
              "${CMAKE_BINARY_DIR}/m6-opus" 1)
  endif()
  add_test(NAME m6-hle
    COMMAND python -I "${CMAKE_SOURCE_DIR}/tools/check_hle.py" "${CMAKE_SOURCE_DIR}/build/m4-target/release/m6-oracle.exe" $<TARGET_FILE:hle-run>
            "${CMAKE_SOURCE_DIR}/tests/m6/hle" "${CMAKE_BINARY_DIR}/m6-hle")
  add_test(NAME m4-layout-fuzz
    COMMAND layout-check fuzz "${CMAKE_SOURCE_DIR}/build/m4-target/release/victory_ref.dll" 2000 7)
  add_test(NAME m4-sampler-fuzz
    COMMAND layout-check samplers "${CMAKE_SOURCE_DIR}/build/m4-target/release/victory_ref.dll" 200000 7)
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
