cmake_minimum_required(VERSION 3.16)

# Host tests are the default so the ARM32 memory model can move without a Vita
# in the loop. The GXM target is opt-in and requires vitasdk.
option(MKWV_BUILD_VITA "Cross-compile the Vita GXM bring-up" OFF)

if(MKWV_BUILD_VITA)
  if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
      set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "vitasdk toolchain")
    else()
      message(FATAL_ERROR "MKWV_BUILD_VITA=ON requires VITASDK or CMAKE_TOOLCHAIN_FILE")
    endif()
  endif()
endif()

project(wiicompiled-vita LANGUAGES C CXX)

# No build type means no optimisation flags at all (vitasdk's toolchain leaves CMAKE_CXX_FLAGS
# empty), which silently built the runtime, the GX decoder and the HLE layer at -O0. Default
# to Release; the Vita targets below also pin their own -O level so this cannot regress.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

enable_testing()

add_subdirectory(vita)

if(NOT MKWV_BUILD_VITA)
  add_subdirectory(tests)
endif()
