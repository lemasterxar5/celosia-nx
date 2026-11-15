cmake_minimum_required(VERSION 3.21)
project(CelosiaSwitchAOT LANGUAGES CXX)
option(CELOSIA_AOT_NATIVE_ONLY "Compile against the separate native runtime ABI" OFF)

# This compiles private generated game code only. It does not link a runtime,
# emulator, graphics API, executable or playable Switch application.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "This target requires an AArch64 cross toolchain")
endif()
set(CELOSIA_RUNNER_DIR "" CACHE PATH "Full generated runner directory, kept private")
set(CELOSIA_SSE2NEON_DIR "" CACHE PATH "sse2neon v1.9.1 include directory")
if(NOT EXISTS "${CELOSIA_RUNNER_DIR}/ps2_recompiled_functions.h" OR
   NOT EXISTS "${CELOSIA_RUNNER_DIR}/register_functions.cpp")
    message(FATAL_ERROR "Generate the complete game before configuring this target")
endif()
if(NOT EXISTS "${CELOSIA_SSE2NEON_DIR}/sse2neon.h")
    message(FATAL_ERROR "Missing pinned sse2neon include directory")
endif()
get_filename_component(CELOSIA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(CELOSIA_RUNTIME "${CELOSIA_ROOT}/PS2Recomp/ps2xRuntime")
file(GLOB CELOSIA_AOT_SOURCES CONFIGURE_DEPENDS "${CELOSIA_RUNNER_DIR}/*.cpp")
# The generated sparse function-table initializer is several MB of assignments.
# It runs at startup, not per frame. Keep it outside unity/O3 to avoid spending
# most of the build optimizing registration together with unrelated game code.
set_source_files_properties("${CELOSIA_RUNNER_DIR}/register_functions.cpp" PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-O1")
add_library(celosia_game_aot STATIC ${CELOSIA_AOT_SOURCES})
target_compile_features(celosia_game_aot PUBLIC cxx_std_20)
target_compile_definitions(celosia_game_aot PRIVATE __SWITCH__ USE_SSE2NEON)
if(CELOSIA_AOT_NATIVE_ONLY)
    target_compile_definitions(celosia_game_aot PRIVATE PS2X_SWITCH_NATIVE_ONLY=1
        PS2X_GUEST_READ_WATCH=0 PS2X_GUEST_WRITE_TRACE=0)
    target_include_directories(celosia_game_aot PRIVATE "${CELOSIA_RUNTIME}/platform/switch")
endif()
target_compile_options(celosia_game_aot PRIVATE
    -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
    -ffunction-sections -fdata-sections)
target_include_directories(celosia_game_aot PRIVATE
    "${CELOSIA_RUNNER_DIR}" "${CELOSIA_SSE2NEON_DIR}"
    "${CELOSIA_RUNTIME}/include" "${CELOSIA_RUNTIME}/src/lib/Kernel"
    "${CELOSIA_ROOT}/PS2Recomp/ps2xIOP/include")
target_precompile_headers(celosia_game_aot PRIVATE "${CELOSIA_RUNTIME}/include/ps2_runtime_macros.h")
set_target_properties(celosia_game_aot PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 32)
include("${CMAKE_CURRENT_LIST_DIR}/../optimization.cmake")
celosia_optimize_compile(celosia_game_aot)
