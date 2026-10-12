# Build vitaGL + vitaShaRK + math-neon for Vita3K.
#
# DriftDroid's GLES path is Aurora/Dawn WebGPU with OpenGL ES as a Dawn backend.
# Dawn will not compile for Vita ARM32 this pass. VitaGL gives us a real GLES 2.0
# surface (and a GLSL translator) on top of sceGxm, which is what we host Aurora's
# GX semantics on.
#
# HAVE_VITA3K_SUPPORT:
#   vitaGL Makefile flag: -DHAVE_VITA3K_SUPPORT -DDISABLE_HW_ETC1
#   We also force shark_init_simple() (vitaShaRK's Vita3K entry) instead of
#   shark_init(), which enables taiHEN SceShaccCgExt. Need libshacccg.suprx at
#   ur0:data/libshacccg.suprx (or ur0:data/external/libshacccg.suprx) inside
#   Vita3K's fs, same as hardware.

set(MKWV_VITAGL_ROOT "${CMAKE_SOURCE_DIR}/third_party")
set(MKWV_VITAGL_DIR "${MKWV_VITAGL_ROOT}/vitaGL")
set(MKWV_VITASHARK_DIR "${MKWV_VITAGL_ROOT}/vitaShaRK")
set(MKWV_MATHNEON_DIR "${MKWV_VITAGL_ROOT}/math-neon")

if(NOT EXISTS "${MKWV_VITAGL_DIR}/source/vitaGL.h")
  message(FATAL_ERROR "vitaGL is missing. Run scripts/fetch-deps.sh, or clone it with:\n"
    "  git clone --depth 1 https://github.com/Rinnegatamante/vitaGL.git ${MKWV_VITAGL_DIR}\n"
    "  git clone --depth 1 https://github.com/Rinnegatamante/vitaShaRK.git ${MKWV_VITASHARK_DIR}\n"
    "  git clone --depth 1 https://github.com/Rinnegatamante/math-neon.git ${MKWV_MATHNEON_DIR}")
endif()

file(GLOB MKWV_MATHNEON_SOURCES "${MKWV_MATHNEON_DIR}/source/*.c")
add_library(mkwv_mathneon STATIC ${MKWV_MATHNEON_SOURCES})
target_include_directories(mkwv_mathneon PUBLIC "${MKWV_MATHNEON_DIR}/source")
target_compile_options(mkwv_mathneon PRIVATE -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize)

add_library(mkwv_vitashark STATIC "${MKWV_VITASHARK_DIR}/source/vitashark.c")
target_include_directories(mkwv_vitashark PUBLIC "${MKWV_VITASHARK_DIR}/source")
target_compile_definitions(mkwv_vitashark PRIVATE HAVE_VITA3K_SUPPORT)
target_compile_options(mkwv_vitashark PRIVATE -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize)

file(GLOB MKWV_VITAGL_C "${MKWV_VITAGL_DIR}/source/*.c" "${MKWV_VITAGL_DIR}/source/utils/*.c")
file(GLOB MKWV_VITAGL_CXX "${MKWV_VITAGL_DIR}/source/utils/*.cpp" "${MKWV_VITAGL_DIR}/source/utils/preprocessor/*.cpp")
add_library(mkwv_vitagl STATIC ${MKWV_VITAGL_C} ${MKWV_VITAGL_CXX})
target_include_directories(mkwv_vitagl PUBLIC
  "${MKWV_VITAGL_DIR}/source"
  "${MKWV_VITASHARK_DIR}/source"
  "${MKWV_MATHNEON_DIR}/source"
)
target_include_directories(mkwv_vitagl PRIVATE
  "${MKWV_VITAGL_DIR}/source"
  "${MKWV_VITAGL_DIR}/source/utils"
  "${MKWV_VITAGL_DIR}/source/utils/preprocessor"
)
target_compile_definitions(mkwv_vitagl PRIVATE
  HAVE_VITA3K_SUPPORT
  DISABLE_HW_ETC1
  SKIP_SPLASHSCREEN
  HAVE_SINGLE_THREADED_GC
  # Keep depth/stencil across GXM scenes. Every EFB copy ends the scene (the readback has to
  # wait for the GPU), and without this the depth buffer came back cleared afterwards, so
  # anything drawn after a copy lost its depth test against what was drawn before it.
  STORE_DEPTH_STENCIL
  # No per-call GL argument validation (vitaGL's NO_DEBUG). The renderer never relies on GL
  # errors to work; this is pure per-call overhead on thousands of calls a frame.
  SKIP_ERROR_HANDLING
  # static73: vitaGL's on-disk shader cache. Every compiled Cg shader is kept as a .gxp under
  # ux0:data/shader_cache/MKWV00001/, keyed by a hash of its source, and later runs load it
  # instead of running SceShaccCg again (a TEV shader takes a good part of a second to compile on
  # a real Vita, and a race start used to compile a couple of hundred of them).
  HAVE_SHADER_CACHE
  VGL_GIT_HASH="mkwv"
)
target_compile_options(mkwv_vitagl PRIVATE
  -O3 -ffast-math -mtune=cortex-a9 -mfpu=neon -mfp16-format=ieee
  -Wno-incompatible-pointer-types -Wno-stringop-overflow -Wno-unused-variable
  -Wno-unused-function -Wno-strict-aliasing
  $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -std=gnu++11 -Wno-write-strings -fpermissive>
)
target_link_libraries(mkwv_vitashark PUBLIC SceShaccCg_stub)
target_link_libraries(mkwv_vitagl PUBLIC mkwv_vitashark mkwv_mathneon
  SceGxm_stub
  SceDisplay_stub
  SceShaccCg_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  SceSysmodule_stub
  SceKernelDmacMgr_stub
)
