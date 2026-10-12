add_library(mkwv_compact_memory STATIC src/runtime/compact_memory.cpp)
add_library(mkwv::compact_memory ALIAS mkwv_compact_memory)
target_include_directories(mkwv_compact_memory PUBLIC include)
target_compile_features(mkwv_compact_memory PUBLIC cxx_std_17)

add_library(mkwv_gx STATIC
  src/gx/state.cpp
  src/gx/fifo.cpp
  src/gx/decode.cpp
  src/gx/demo.cpp
  src/gx/texture.cpp
  src/gx/tev.cpp
  src/gx/cg_emit.cpp
  src/gx/capture.cpp
  src/gx/perf.cpp
  src/gx/vertex_worker.cpp
  src/gx/dl_cache.cpp
  src/gx/gpu_mesh.cpp
)
add_library(mkwv::gx ALIAS mkwv_gx)
target_include_directories(mkwv_gx PUBLIC include)
target_compile_features(mkwv_gx PUBLIC cxx_std_17)

add_library(mkwv_runtime STATIC
  src/runtime/overlay.cpp
  src/runtime/hle_fifo.cpp
  src/runtime/hle_boot.cpp
  src/runtime/hle_os.cpp
  src/runtime/hle_dvd.cpp
  src/runtime/hle_gx.cpp
  src/runtime/hle_native.cpp
  src/runtime/guest_access.cpp
  src/runtime/guest_image.cpp
  src/runtime/dispatch.cpp
  src/runtime/diagnostics.cpp
  src/runtime/runtime.cpp
  src/runtime/ppc_helpers.cpp
  src/runtime/fpu_helpers.cpp
  src/runtime/audio/audio.cpp
  src/runtime/guest_prof.cpp
  src/runtime/guest_sync.cpp
  src/runtime/engine_native.cpp
  src/runtime/audio/ax_mix.cpp
  src/runtime/audio/ax_memory.cpp
)
add_library(mkwv::runtime ALIAS mkwv_runtime)
target_include_directories(mkwv_runtime PUBLIC include)
target_compile_features(mkwv_runtime PUBLIC cxx_std_17)
target_link_libraries(mkwv_runtime PUBLIC mkwv::compact_memory mkwv::gx)
if(MKWV_BUILD_VITA)
  target_compile_options(mkwv_runtime PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
  # Explicit optimisation for the runtime libraries. Without a build type these were -O0:
  # the GX decoder (every vertex transformed and lit on the CPU), texture decoding, the HLE
  # layer and guest dispatch all ran unoptimised.
  foreach(lib mkwv_compact_memory mkwv_gx mkwv_runtime)
    target_compile_options(${lib} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O2 -fno-strict-aliasing>)
  endforeach()
  # The per-vertex loop. -O3 and no libm errno lets sqrt become one instruction; the file
  # stays strict about aliasing. Host tests keep the default flags and the scalar path.
  set_source_files_properties(src/gx/decode.cpp PROPERTIES
    COMPILE_OPTIONS "-O3;-fno-strict-aliasing;-fno-math-errno;-mfpu=neon")
  set_source_files_properties(src/gx/dl_cache.cpp src/gx/vertex_worker.cpp PROPERTIES
    COMPILE_OPTIONS "-O3;-fno-strict-aliasing;-mfpu=neon")
  # The AX/DSP mix runs 333 times a second on the sound thread (static61 real Vita: ~26% of a
  # core). NEON kernels in ax_mix_kernels.h.
  set_source_files_properties(src/runtime/audio/ax_mix.cpp PROPERTIES
    COMPILE_OPTIONS "-O3;-fno-strict-aliasing;-mfpu=neon")
  set_source_files_properties(src/runtime/engine_native.cpp PROPERTIES
    COMPILE_OPTIONS "-O3;-fno-strict-aliasing;-fno-math-errno;-ffp-contract=off;-mfpu=neon")
endif()

if(NOT MKWV_BUILD_VITA)
  return()
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vitaGL.cmake")

enable_language(ASM)

set(VITA_APP_NAME "WiiCompiled Vita")
set(VITA_TITLEID "MKWV00001")
set(VITA_VERSION "00.01")

set(MKWV_HAVE_TRANSLATED 0)
set(MKWV_SHARDS_CMAKE "${CMAKE_SOURCE_DIR}/upstream/generated/build_shards/shards.cmake")
if(EXISTS "${MKWV_SHARDS_CMAKE}")
  include("${MKWV_SHARDS_CMAKE}")
  if(MKW_BASE_COMMON_SHARDS)
    # Forced inlining is gone for good (memory accessors, compare/FP helpers, always_inline ISA
    # helpers). Clear stale cache values too, so old configure commands cannot turn it back on.
    set(MKWV_SHARD_MEMORY_INLINE OFF CACHE BOOL "Removed: guest RAM inlining" FORCE)
    set(MKWV_SHARD_MEMORY_INLINE_ALL OFF CACHE BOOL "Removed: guest RAM inlining" FORCE)
    set(MKWV_SHARD_HOT_INLINE OFF CACHE BOOL "Removed: forced compare/FP helper inlining" FORCE)
    message(STATUS "mkwv: forced memory and hot-helper inlining disabled")

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MKWV_ENGINE_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/include/mkwv/runtime/engine_native_catalog.inc")
    set(MKWV_ENGINE_PREPARE "${CMAKE_SOURCE_DIR}/scripts/prepare-engine-shards.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MKWV_ENGINE_CATALOG}" "${MKWV_ENGINE_PREPARE}")
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" "${MKWV_ENGINE_PREPARE}"
        "${CMAKE_SOURCE_DIR}/upstream/generated/build_shards/base_common"
        "${MKWV_ENGINE_CATALOG}" "${CMAKE_CURRENT_BINARY_DIR}/engine_shards"
        "${CMAKE_CURRENT_BINARY_DIR}/engine_shards.cmake"
      RESULT_VARIABLE MKWV_ENGINE_PREPARE_RESULT)
    if(NOT MKWV_ENGINE_PREPARE_RESULT EQUAL 0)
      message(FATAL_ERROR "Native engine shard preparation failed")
    endif()
    include("${CMAKE_CURRENT_BINARY_DIR}/engine_shards.cmake")

    # Same FP contract as upstream's MKW_TRANSLATED_PPC_FP_OPTIONS: the translator's explicit float
    # casts are PPC single-precision rounding points, and fast-math (finite-math-only, reassociation,
    # crtfastmath's FTZ) breaks fcmp/NaN handling and guest-visible integer conversions.
    # -O2 by default (was -Os): the translated code is where most CPU time goes, and -Os turns
    # off scheduling and the inlining of the guest-memory accessors' fast paths. Pass
    # -DMKWV_SHARD_OPT=-Os to go back (e.g. if I-cache pressure turns out to dominate).
    set(MKWV_SHARD_OPT "-O2" CACHE STRING "Optimisation level for the translated shards")
    function(mkwv_setup_translated target)
      target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/upstream/generated/build_shards/base_common"
      )
      target_compile_features(${target} PRIVATE cxx_std_17)
      target_compile_options(${target} PRIVATE
        ${MKWV_SHARD_OPT} -fno-fast-math -ffp-contract=off -fno-tree-slp-vectorize -fno-exceptions -fno-rtti -w -pipe
      )
    endfunction()

    add_library(mkwv_translated OBJECT
      ${MKWV_ENGINE_SHARDS}
      "${CMAKE_SOURCE_DIR}/upstream/generated/build_shards/base_dispatch/indirect_dispatch_a0a39a429c344b0a.cpp"
    )
    mkwv_setup_translated(mkwv_translated)
    # Fast builds must examine only objects used by this target. Old source paths
    # and removed static67 object libraries can leave stale .obj files behind.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/translated-objects.txt"
      CONTENT "$<JOIN:$<TARGET_OBJECTS:mkwv_translated>,\n>\n")
    set(MKWV_HAVE_TRANSLATED 1)

    # Game functions the shard graph omits (upstream replaces them natively on PC) whose
    # translated code is what the Vita wants. Generated from the user's base translation at
    # build time; registers itself with the runtime (mkwv_register_extra_native).
    #   8022E2BC EGG::LightTexture::SetupTevFinish
    set(MKWV_EXTRA_TRANSLATED 8022E2BC)
    set(MKWV_TRANSLATION_SOURCES "${CMAKE_SOURCE_DIR}/upstream/generated/base_translation_sources.bin")
    find_package(Python3 COMPONENTS Interpreter)
    if(EXISTS "${MKWV_TRANSLATION_SOURCES}" AND Python3_Interpreter_FOUND)
      set(MKWV_EXTRA_TRANSLATED_CPP "${CMAKE_CURRENT_BINARY_DIR}/extra_translated.cpp")
      add_custom_command(
        OUTPUT "${MKWV_EXTRA_TRANSLATED_CPP}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/extract_translated.py"
                "${MKWV_TRANSLATION_SOURCES}" "${MKWV_EXTRA_TRANSLATED_CPP}" ${MKWV_EXTRA_TRANSLATED}
        DEPENDS "${CMAKE_SOURCE_DIR}/scripts/extract_translated.py" "${MKWV_TRANSLATION_SOURCES}"
        COMMENT "Extracting translated functions outside the shard graph"
        VERBATIM
      )
      target_sources(mkwv_translated PRIVATE "${MKWV_EXTRA_TRANSLATED_CPP}")
    else()
      message(WARNING "base_translation_sources.bin or python3 missing: SetupTevFinish stays unimplemented")
    endif()
  endif()
endif()

add_executable(wiicompiled-vita
  src/main.cpp
  src/gx/gl_backend.cpp
  src/gx/gl_submit.cpp
  src/platform/clocks.cpp
  src/runtime/host_context.cpp
  src/platform/vita/render_thread.cpp
  src/platform/vita/audio_out.cpp
  src/platform/vita/profiler_thread.cpp
  src/platform/vita/host_profiler.cpp
  src/platform/vita/sound_thread.cpp
  src/platform/vita/co_switch.S
  src/platform/vita/heap_size.c
)
target_include_directories(wiicompiled-vita PRIVATE include)
target_compile_definitions(wiicompiled-vita PRIVATE MKWV_HAVE_TRANSLATED=${MKWV_HAVE_TRANSLATED})
target_compile_options(wiicompiled-vita PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-O3 -ffast-math -fno-finite-math-only -fno-exceptions -fno-rtti>
  $<$<COMPILE_LANGUAGE:C>:-O3>
)
target_link_libraries(wiicompiled-vita PRIVATE
  mkwv::compact_memory
  mkwv::gx
  mkwv::runtime
  $<$<BOOL:${MKWV_HAVE_TRANSLATED}>:mkwv_translated>
  mkwv_vitagl
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  ScePower_stub
  SceSysmem_stub
  SceLibKernel_stub
  SceIofilemgr_stub
  SceShaccCg_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceSysmodule_stub
  SceKernelDmacMgr_stub
  SceAudio_stub
  m
)
# .init_array is pinned (so it does not overlap vitaGL's rodata), which starts the data
# segment there too. It has to sit above the end of .text: when the code grows past it the
# link fails with ".init_array LMA ... overlaps section .text LMA [81000040,<end>]" - raise
# this above <end>. The gap costs no memory (the loader places each segment on its own).
# 0x82A00000 held the -Os shards; the -O2 shards with inlined guest-memory access end at
# about 0x832CA000. Aggressive RAM inlining can roughly double text; static66 uses
# 0x88000000 so the full rebuild cannot overlap the data segment.
set(MKWV_INIT_ARRAY_ADDR "0x88000000" CACHE STRING "Load address of .init_array (must be above the end of .text)")
target_link_options(wiicompiled-vita PRIVATE
  -Wl,-q
  -Wl,-z,max-page-size=0x10000
  -Wl,--section-start,.init_array=${MKWV_INIT_ARRAY_ADDR}
)

vita_create_self(wiicompiled-vita.self wiicompiled-vita UNSAFE)
# Extended memory mode: ATTRIBUTE2=12 asks the system for the larger application memory budget
# (about 109 MiB more user memory). A stock budget leaves ~80 MiB free after the executable and
# the libc heap, which cannot hold the guest's 90 MiB of RAM plus vitaGL.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
# Dolphin's free DSP coefficient ROM (GPL, not a Nintendo file; see upstream's
# THIRD-PARTY-NOTICES.md), taken from the upstream checkout at build time.
set(MKWV_DSP_COEF "${CMAKE_SOURCE_DIR}/upstream/runtime/assets/dsp/dsp_coef.bin")
set(MKWV_VPK_FILES)
if(EXISTS "${MKWV_DSP_COEF}")
  list(APPEND MKWV_VPK_FILES FILE "${MKWV_DSP_COEF}" dsp_coef.bin)
else()
  message(WARNING "upstream/runtime/assets/dsp/dsp_coef.bin missing: AX voices will resample linearly")
endif()
vita_create_vpk(wiicompiled-vita.vpk ${VITA_TITLEID} wiicompiled-vita.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  ${MKWV_VPK_FILES}
)
