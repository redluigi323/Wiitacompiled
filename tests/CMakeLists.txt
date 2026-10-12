add_executable(mkwv_compact_memory_tests compact_memory_tests.cpp)
target_link_libraries(mkwv_compact_memory_tests PRIVATE mkwv::compact_memory)
add_test(NAME mkwv_compact_memory_tests COMMAND mkwv_compact_memory_tests)

add_executable(mkwv_gx_fifo_tests gx_fifo_tests.cpp)
target_link_libraries(mkwv_gx_fifo_tests PRIVATE mkwv::gx)
add_test(NAME mkwv_gx_fifo_tests COMMAND mkwv_gx_fifo_tests)

add_executable(mkwv_gx_cg_tests gx_cg_tests.cpp)
target_link_libraries(mkwv_gx_cg_tests PRIVATE mkwv::gx)
add_test(NAME mkwv_gx_cg_tests COMMAND mkwv_gx_cg_tests)

# Includes vita/src/gx/gl_submit.cpp directly, with tests/stub_gl/vitaGL.h standing in
# for the real header, so the GX TEV -> GL_COMBINE mapping can be checked on the host.
# gl_submit.cpp is not part of mkwv_gx, so there is nothing to collide with.
add_executable(mkwv_gx_tev_combine_tests gx_tev_combine_tests.cpp)
target_include_directories(mkwv_gx_tev_combine_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stub_gl")
target_link_libraries(mkwv_gx_tev_combine_tests PRIVATE mkwv::gx)
add_test(NAME mkwv_gx_tev_combine_tests COMMAND mkwv_gx_tev_combine_tests)

add_executable(mkwv_overlay_tests overlay_tests.cpp)
target_link_libraries(mkwv_overlay_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_overlay_tests COMMAND mkwv_overlay_tests)

add_executable(mkwv_memory_inline_tests memory_inline_tests.cpp)
target_link_libraries(mkwv_memory_inline_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_memory_inline_tests COMMAND mkwv_memory_inline_tests)

add_executable(mkwv_translated_isa_tests translated_isa_tests.cpp)
target_link_libraries(mkwv_translated_isa_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_translated_isa_tests COMMAND mkwv_translated_isa_tests)

add_executable(mkwv_rel_lifecycle_tests rel_lifecycle_tests.cpp)
target_link_libraries(mkwv_rel_lifecycle_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_rel_lifecycle_tests COMMAND mkwv_rel_lifecycle_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EXP_HEAP_SHARD "${CMAKE_SOURCE_DIR}/upstream/generated/build_shards/base_common/shard_ed60942cd5fc653538845a05.cpp")
set(EXP_HEAP_REFERENCE "${CMAKE_CURRENT_BINARY_DIR}/exp_heap_reference.cpp")
add_custom_command(OUTPUT "${EXP_HEAP_REFERENCE}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_exp_heap_reference.py"
          "${EXP_HEAP_SHARD}" "${EXP_HEAP_REFERENCE}"
  DEPENDS "${EXP_HEAP_SHARD}" extract_exp_heap_reference.py)
add_executable(mkwv_exp_heap_tests exp_heap_tests.cpp "${EXP_HEAP_REFERENCE}")
target_link_libraries(mkwv_exp_heap_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_exp_heap_tests COMMAND mkwv_exp_heap_tests)


set(MKWV_GUEST_BLOBS "${CMAKE_SOURCE_DIR}/upstream/generated/data_sections_init_blobs")
if(EXISTS "${MKWV_GUEST_BLOBS}/StaticR.bin")
  add_executable(mkwv_guest_image_tests guest_image_tests.cpp)
  target_link_libraries(mkwv_guest_image_tests PRIVATE mkwv::runtime)
  target_compile_definitions(mkwv_guest_image_tests PRIVATE MKWV_GUEST_BLOBS_DIR="${MKWV_GUEST_BLOBS}")
  add_test(NAME mkwv_guest_image_tests COMMAND mkwv_guest_image_tests)
endif()

# GX HLE natives -> FIFO -> decoder, end to end (the hle_gx.cpp / fifo.cpp register contract).
add_executable(mkwv_gx_hle_pipeline_tests gx_hle_pipeline_tests.cpp)
target_link_libraries(mkwv_gx_hle_pipeline_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_gx_hle_pipeline_tests COMMAND mkwv_gx_hle_pipeline_tests)

# Host replay of an on-device frame capture (not a test): see tools/gx_replay.cpp.
add_executable(mkwv_gx_replay "${CMAKE_SOURCE_DIR}/tools/gx_replay.cpp")
target_link_libraries(mkwv_gx_replay PRIVATE mkwv::gx)

add_executable(mkwv_gx_vertex_reuse_tests gx_vertex_reuse_tests.cpp)
find_package(Threads REQUIRED)
target_link_libraries(mkwv_gx_vertex_reuse_tests PRIVATE mkwv::gx Threads::Threads)
add_test(NAME mkwv_gx_vertex_reuse_tests COMMAND mkwv_gx_vertex_reuse_tests)

add_executable(mkwv_audio_hle_tests audio_hle_tests.cpp)
target_link_libraries(mkwv_audio_hle_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_audio_hle_tests COMMAND mkwv_audio_hle_tests)

add_executable(mkwv_guest_fiber_tests guest_fiber_tests.cpp)
target_link_libraries(mkwv_guest_fiber_tests PRIVATE mkwv::runtime)
add_test(NAME mkwv_guest_fiber_tests COMMAND mkwv_guest_fiber_tests)

add_executable(mkwv_sound_thread_tests sound_thread_tests.cpp)
target_link_libraries(mkwv_sound_thread_tests PRIVATE mkwv::runtime)
find_package(Threads REQUIRED)
target_link_libraries(mkwv_sound_thread_tests PRIVATE Threads::Threads)
add_test(NAME mkwv_sound_thread_tests COMMAND mkwv_sound_thread_tests)

add_executable(mkwv_gx_gpu_lit_tests gx_gpu_lit_tests.cpp)
target_link_libraries(mkwv_gx_gpu_lit_tests PRIVATE mkwv::gx Threads::Threads)
add_test(NAME mkwv_gx_gpu_lit_tests COMMAND mkwv_gx_gpu_lit_tests)

add_executable(mkwv_gx_gpu_xf_tests gx_gpu_xf_tests.cpp)
target_link_libraries(mkwv_gx_gpu_xf_tests PRIVATE mkwv::gx Threads::Threads)
add_test(NAME mkwv_gx_gpu_xf_tests COMMAND mkwv_gx_gpu_xf_tests)

add_executable(mkwv_gx_dl_cache_tests gx_dl_cache_tests.cpp)
target_link_libraries(mkwv_gx_dl_cache_tests PRIVATE mkwv::gx Threads::Threads)
add_test(NAME mkwv_gx_dl_cache_tests COMMAND mkwv_gx_dl_cache_tests)

# macOS hides its ucontext declarations unless X/Open compatibility is requested.
if(APPLE)
  target_compile_definitions(mkwv_guest_fiber_tests PRIVATE _XOPEN_SOURCE=700)
  target_compile_definitions(mkwv_sound_thread_tests PRIVATE _XOPEN_SOURCE=700)
endif()

add_executable(mkwv_audio_kernel_tests audio_kernel_tests.cpp)
target_include_directories(mkwv_audio_kernel_tests PRIVATE "${CMAKE_SOURCE_DIR}/vita/include")
add_test(NAME mkwv_audio_kernel_tests COMMAND mkwv_audio_kernel_tests)

if(EXISTS "${MKWV_GUEST_BLOBS}/StaticR.bin")
  set(MKWV_ENGINE_REFERENCE "${CMAKE_CURRENT_BINARY_DIR}/engine_reference.cpp")
  set(MKWV_ENGINE_CATALOG "${CMAKE_SOURCE_DIR}/vita/include/mkwv/runtime/engine_native_catalog.inc")
  add_custom_command(OUTPUT "${MKWV_ENGINE_REFERENCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_engine_reference.py"
      "${CMAKE_SOURCE_DIR}/upstream/generated/base_translation_sources.bin"
      "${MKWV_ENGINE_CATALOG}" "${MKWV_ENGINE_REFERENCE}"
    DEPENDS extract_engine_reference.py "${MKWV_ENGINE_CATALOG}"
      "${CMAKE_SOURCE_DIR}/upstream/generated/base_translation_sources.bin")
  add_executable(mkwv_engine_native_tests engine_native_tests.cpp "${MKWV_ENGINE_REFERENCE}")
  target_link_libraries(mkwv_engine_native_tests PRIVATE mkwv::runtime)
  target_compile_definitions(mkwv_engine_native_tests PRIVATE MKWV_GUEST_BLOBS_DIR="${MKWV_GUEST_BLOBS}")
  add_test(NAME mkwv_engine_native_tests COMMAND mkwv_engine_native_tests)
endif()
