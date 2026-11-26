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

add_library(danzi_formats STATIC src/formats/texture.cpp src/formats/bcpack.cpp)
target_include_directories(danzi_formats PUBLIC src)
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

add_library(danzi_pe STATIC src/pe/image.cpp src/pe/implib.cpp)
target_include_directories(danzi_pe PUBLIC src)
add_library(danzi_loader STATIC src/loader/prepare.cpp src/loader/windows.cpp src/loader/threads.cpp src/loader/tls.cpp
  src/loader/hooks.cpp src/loader/crash.cpp src/loader/emulate.cpp)
target_link_libraries(danzi_loader PUBLIC danzi_pe danzi_core blake3)
add_library(danzi_hle STATIC src/hle/registry.cpp src/hle/table.cpp src/hle/runtime.cpp src/hle/kernel.cpp
  src/hle/xgameruntime.cpp src/hle/areas.cpp src/hle/xasync.cpp src/hle/xuser.cpp src/hle/xsystem.cpp src/hle/xgame.cpp
  src/hle/areas_small.cpp src/hle/xgamesave.cpp src/hle/xstore.cpp src/hle/xgameui.cpp src/hle/files.cpp src/hle/lifecycle.cpp
  src/hle/gameinput_cursor.cpp src/hle/gameinput.cpp src/hle/xmem.cpp src/hle/d3d12x.cpp src/hle/dstorage.cpp src/hle/net.cpp src/hle/media.cpp
  src/hle/audio.cpp src/hle/audio_capture.cpp src/hle/apu.cpp src/hle/dsp.cpp src/hle/xg.cpp src/formats/xma.cpp)
target_link_libraries(danzi_hle PUBLIC danzi_loader danzi_d3d12x opus)
add_library(danzi_launcher SHARED apps/victory/main.cpp apps/victory/shader.cpp apps/victory/translate.cpp apps/victory/precompile.cpp
  src/formats/sgd5.cpp src/shader/sp3.cpp src/shader/host.cpp src/shader/validator.cpp)
target_link_libraries(danzi_launcher PRIVATE danzi_hle danzi_core blake3)
target_include_directories(danzi_launcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_executable(victory apps/victory/entry.cpp)
target_link_libraries(victory PRIVATE danzi_launcher)
