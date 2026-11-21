cmake_minimum_required(VERSION 3.21)
project(CelosiaHybridRuntime LANGUAGES CXX)

# Isolated full-runtime ARM archive. This does not alter the native-only core,
# build a runner, implement a host API, or embed private game code/assets.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "The hybrid runtime requires an AArch64 cross compiler")
endif()
get_filename_component(CELOSIA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
get_filename_component(CELOSIA_WORK "${CELOSIA_ROOT}/../.." ABSOLUTE)
set(CELOSIA_RUNTIME "${CELOSIA_ROOT}/PS2Recomp/ps2xRuntime")
get_filename_component(CELOSIA_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(CELOSIA_SDK_DEFAULT "${CELOSIA_COMPILER_BIN}/../.." ABSOLUTE)
set(CELOSIA_DEVKITPRO "${CELOSIA_SDK_DEFAULT}" CACHE PATH "Installed devkitPro root")
set(CELOSIA_SSE2NEON_DIR "${CELOSIA_WORK}/arm-deps" CACHE PATH "Pinned sse2neon v1.9.1 headers")
set(CELOSIA_HYBRID_HOST_INCLUDE "${CELOSIA_RUNTIME}/platform/switch/hybrid" CACHE PATH
    "Host API declarations only; actual Switch implementation must be supplied at link time")
set(CELOSIA_HYBRID_ZLIB "${CELOSIA_DEVKITPRO}/portlibs/switch/lib/libz.a" CACHE FILEPATH
    "AArch64 zlib archive for the existing scene recorder")
foreach(required IN ITEMS "${CELOSIA_DEVKITPRO}/libnx/include/switch.h"
        "${CELOSIA_SSE2NEON_DIR}/sse2neon.h" "${CELOSIA_HYBRID_HOST_INCLUDE}/raylib.h"
        "${CELOSIA_HYBRID_ZLIB}" "${CELOSIA_DEVKITPRO}/portlibs/switch/include/zlib.h")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing hybrid runtime dependency: ${required}")
    endif()
endforeach()

# Explicitly select the full subsystem, including its secondary IOP processor
# and peripheral services. No state leaks from the native-core build directory.
set(PS2X_IOP_NATIVE_ONLY OFF CACHE BOOL "Full secondary subsystem in this target" FORCE)
set(PS2X_IOP_BUILD_TESTS OFF CACHE BOOL "No cross-compiled host tests" FORCE)
set(PS2X_IOP_BUILD_NATIVE_TESTS OFF CACHE BOOL "No native-core tests in this target" FORCE)
add_subdirectory("${CELOSIA_ROOT}/PS2Recomp/ps2xIOP" secondary-services)

# Keep in sync with ps2xRuntime/CMakeLists.txt's ps2_runtime source block.
# Desktop runner/UI dependencies and all GL/Metal implementations are omitted.
set(runtime_sources
    game_overrides.cpp gs/ps2_gif_arbiter.cpp ps2_audio.cpp ps2_audio_vag.cpp
    gs/ps2_gs_memory.cpp gs/gs_frontend.cpp gs/gs_cpu_backend.cpp
    gs/gs_async_backend.cpp gs/gs_hw_backend.cpp ps2_iop_host.cpp ps2_memory.cpp ps2_memory_state.cpp ps2_runtime_state.cpp
    ps2_pad.cpp host_menu.cpp ps2_rom_device.cpp ps2_vfs.cpp ps2_runtime.cpp
    ps2_macro_selftest.cpp ps2_vif1_interpreter.cpp vu/ps2_vu1_core.cpp
    vu/ps2_vu1_upper.cpp vu/ps2_vu1_lower.cpp vu/ps2_vu1_rec.cpp games_database.cpp
    gs/gs_metal_raster_stub.cpp native/native_renderer.cpp native/native_recorder.cpp
    native/deko3d/backend.cpp native/deko3d/frame_mesh.cpp native/deko3d/render.cpp native/deko3d/lsfg.cpp)
list(TRANSFORM runtime_sources PREPEND "${CELOSIA_RUNTIME}/src/lib/")
file(GLOB_RECURSE kernel_sources CONFIGURE_DEPENDS "${CELOSIA_RUNTIME}/src/lib/Kernel/*.cpp")
add_library(celosia_hybrid_runtime STATIC ${runtime_sources} ${kernel_sources})
target_sources(celosia_hybrid_runtime PRIVATE
    "${CELOSIA_RUNTIME}/platform/switch/hybrid/raylib_window.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/hybrid/raylib_input_audio.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/hybrid/raylib_sounds.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/hybrid/switch_menu.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/hybrid/async_log.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/hybrid/save_states.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/native_audio_output.cpp")
target_sources(celosia_hybrid_runtime PRIVATE "${CELOSIA_RUNTIME}/platform/switch/native_faults.cpp")
target_sources(celosia_hybrid_runtime PRIVATE "${CELOSIA_RUNTIME}/platform/switch/hybrid/diagnostics.cpp")
target_sources(celosia_hybrid_runtime PRIVATE "${CELOSIA_RUNTIME}/platform/switch/hybrid/owned_backend.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/native_executor.cpp" "${CELOSIA_RUNTIME}/platform/switch/native_threads.cpp"
    "${CELOSIA_RUNTIME}/platform/switch/native_semaphore.cpp")
target_include_directories(celosia_hybrid_runtime PRIVATE "${CELOSIA_RUNTIME}/platform/switch")
# v173m: lsfg-deko3d runtime (NaGaa95 Cemu-nx 1.3.3, GPL-3.0-or-later): image graph, schedule, bindings, cache.
set(lsfg_deko3d "${CELOSIA_ROOT}/dependencies/lsfg-deko3d")
foreach(f binding layout schedule cache_load cache_format cache_store image_graph error sha256 dksh shader_set spirv_module pe_resources)
    target_sources(celosia_hybrid_runtime PRIVATE "${lsfg_deko3d}/src/${f}.cpp")
endforeach()
target_include_directories(celosia_hybrid_runtime PUBLIC "${lsfg_deko3d}/include")
target_compile_features(celosia_hybrid_runtime PUBLIC cxx_std_20)
target_compile_definitions(celosia_hybrid_runtime PUBLIC __SWITCH__ USE_SSE2NEON PS2X_NATIVE_DEKO3D=1)
target_compile_definitions(celosia_hybrid_runtime PRIVATE PS2X_SWITCH_HYBRID=1)
# No decoder is falsely implemented here. The existing MPEG fallback remains
# a known limitation until an actual ARM video decoder is integrated.
target_compile_definitions(celosia_hybrid_runtime PRIVATE PS2X_HAS_FFMPEG=0 PS2X_ENABLE_IOP_RPC_TRACE=0)
target_include_directories(celosia_hybrid_runtime PUBLIC
    "${CELOSIA_RUNTIME}/include" "${CELOSIA_RUNTIME}/src/lib/Kernel"
    "${CELOSIA_HYBRID_HOST_INCLUDE}" "${CELOSIA_SSE2NEON_DIR}"
    "${CELOSIA_RUNTIME}/platform/switch/hybrid")
target_compile_options(celosia_hybrid_runtime PRIVATE
    -include "${CELOSIA_RUNTIME}/platform/switch/hybrid/host_declarations.hpp")
target_include_directories(celosia_hybrid_runtime SYSTEM PRIVATE
    "${CELOSIA_DEVKITPRO}/libnx/include" "${CELOSIA_DEVKITPRO}/portlibs/switch/include")
add_library(celosia_hybrid_zlib STATIC IMPORTED)
set_target_properties(celosia_hybrid_zlib PROPERTIES IMPORTED_LOCATION "${CELOSIA_HYBRID_ZLIB}")
target_link_libraries(celosia_hybrid_runtime PUBLIC ps2_iop celosia_hybrid_zlib)
foreach(target IN ITEMS celosia_hybrid_runtime ps2_iop)
    target_compile_options(${target} PRIVATE -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
        -ffunction-sections -fdata-sections)
endforeach()
message(STATUS "Hybrid archive uses full secondary services and direct deko3d; host API implementation and video decoder remain external")

set(CELOSIA_HYBRID_GAME_ARCHIVE "${CELOSIA_WORK}/aarch64-game/libcelosia_game_aot.a" CACHE FILEPATH "Full compatible private ARM game archive")
set(CELOSIA_HYBRID_GAME_HEADERS "${CELOSIA_WORK}/game-complete/output" CACHE PATH "Private generated game headers")
file(GLOB override_sources CONFIGURE_DEPENDS "${CELOSIA_ROOT}/game/overrides/*.cpp")
add_library(celosia_hybrid_overrides STATIC ${override_sources})
target_link_libraries(celosia_hybrid_overrides PUBLIC celosia_hybrid_runtime)
target_include_directories(celosia_hybrid_overrides PRIVATE "${CELOSIA_HYBRID_GAME_HEADERS}")
target_compile_options(celosia_hybrid_overrides PRIVATE -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fdata-sections)
target_include_directories(celosia_hybrid_overrides SYSTEM PRIVATE "${CELOSIA_DEVKITPRO}/libnx/include")
add_executable(celosia_switch_game "${CELOSIA_RUNTIME}/platform/switch/hybrid/main.cpp")
set_target_properties(celosia_switch_game PROPERTIES SUFFIX ".elf")
target_include_directories(celosia_switch_game SYSTEM PRIVATE "${CELOSIA_DEVKITPRO}/libnx/include")
target_include_directories(celosia_switch_game PRIVATE "${CELOSIA_RUNTIME}/platform/switch")
target_compile_options(celosia_switch_game PRIVATE -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE)
target_link_directories(celosia_switch_game PRIVATE "${CELOSIA_DEVKITPRO}/libnx/lib" "${CELOSIA_DEVKITPRO}/portlibs/switch/lib")
target_link_options(celosia_switch_game PRIVATE -fPIC "-specs=${CELOSIA_DEVKITPRO}/libnx/switch.specs" -Wl,--gc-sections "-Wl,-Map,${CMAKE_BINARY_DIR}/celosia_switch_game.map")
target_link_options(celosia_switch_game PRIVATE -Wl,--wrap=nwindowReleaseBuffers -Wl,--wrap=nwindowClose -Wl,--wrap=abort)
target_link_options(celosia_switch_game PRIVATE -Wl,--wrap=exit -Wl,--wrap=_Exit -Wl,--wrap=_exit -Wl,--wrap=__libnx_exit)
target_link_options(celosia_switch_game PRIVATE -Wl,--wrap=nvMapCreate -Wl,--wrap=nvMapClose -Wl,--wrap=nvAddressSpaceMap)
option(CELOSIA_HYBRID_RELOCATION_AUDIT "Produce an ELF solely to inspect unexpected read-only relocations" OFF)
if(CELOSIA_HYBRID_RELOCATION_AUDIT)
    target_link_options(celosia_switch_game PRIVATE -Wl,--warn-shared-textrel -Wl,-z,notext)
endif()
option(CELOSIA_DEKO_DIAGNOSTICS "Use deko3d SDK validation and detailed fatal messages" OFF)
set(celosia_deko_library deko3d)
if(CELOSIA_DEKO_DIAGNOSTICS)
    set(celosia_deko_library deko3dd)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../optimization.cmake")
foreach(target IN ITEMS celosia_hybrid_runtime ps2_iop celosia_hybrid_overrides celosia_switch_game)
    celosia_optimize_compile(${target})
endforeach()
if(CELOSIA_OPT_LINK)
    target_link_options(celosia_switch_game PRIVATE ${CELOSIA_OPT_LINK})
endif()
# SaltyNX FPS counter block for overlays (no effect when SaltyNX is absent).
target_sources(celosia_switch_game PRIVATE "${CELOSIA_RUNTIME}/platform/switch/hybrid/saltynx_fps.cpp")
if(CELOSIA_PGO STREQUAL "GENERATE")
    target_sources(celosia_switch_game PRIVATE "${CELOSIA_RUNTIME}/platform/switch/hybrid/pgo_dump.cpp")
    set_source_files_properties("${CELOSIA_RUNTIME}/platform/switch/hybrid/pgo_dump.cpp" PROPERTIES
        COMPILE_DEFINITIONS "CELOSIA_PGO_DIR=\"${CELOSIA_PGO_DIR}\"")
    target_link_options(celosia_switch_game PRIVATE -Wl,--wrap=fopen)
endif()
target_link_libraries(celosia_switch_game PRIVATE -Wl,--start-group -Wl,--whole-archive
    celosia_hybrid_overrides "${CELOSIA_HYBRID_GAME_ARCHIVE}" -Wl,--no-whole-archive
    celosia_hybrid_runtime ps2_iop ${celosia_deko_library} "${CELOSIA_DEVKITPRO}/portlibs/switch/lib/libzstd.a" nx m -Wl,--end-group)
