# Optional whole-program optimisation for the Switch targets (AOT game archive, hybrid runtime,
# overrides and the executable). Defaults keep the previous build unchanged.
#
#   CELOSIA_LTO=ON          -flto IR objects (archives need gcc-ar/gcc-ranlib) and a parallel LTO link.
#   CELOSIA_PGO=GENERATE    instrumented build; profiles are written on the console under
#                           CELOSIA_PGO_DIR (an sdmc: path) by platform/switch/hybrid/pgo_dump.cpp.
#   CELOSIA_PGO=USE         optimise with the .gcda files collected into CELOSIA_PGO_DIR (local path).
#
# Profile names: GCC mangles "<build dir>\<object>" into the .gcda name. Each project strips its own
# build directory (-fprofile-prefix-path, native separators as returned by getpwd on Windows), so
# names are "CMakeFiles#<target>.dir#...#<object>.gcda" and match between GENERATE and USE builds
# located in different build roots.
set(CELOSIA_PGO "OFF" CACHE STRING "Profile-guided optimisation: OFF, GENERATE or USE")
set_property(CACHE CELOSIA_PGO PROPERTY STRINGS OFF GENERATE USE)
option(CELOSIA_LTO "Link-time optimisation across game code, runtime and overrides" OFF)
set(CELOSIA_PGO_DIR "" CACHE STRING "GENERATE: on-console sdmc: directory; USE: local directory with .gcda files")
set(CELOSIA_LTO_JOBS "6" CACHE STRING "Parallel LTRANS jobs for the LTO link")
# runtime: profile only the runtime, renderer and overrides. Instrumenting the ~9000 recompiled game
# functions doubles the image (.text 49->112 MB) and that build hangs on the console before its
# first frame; the AOT archive then keeps plain -O3 (+LTO) and is not profiled.
set(CELOSIA_PGO_SCOPE "all" CACHE STRING "PGO scope: all or runtime")
set_property(CACHE CELOSIA_PGO_SCOPE PROPERTY STRINGS all runtime)

file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" celosia_profile_prefix)
set(CELOSIA_OPT_COMPILE "")
set(CELOSIA_LTO_COMPILE "")
set(CELOSIA_OPT_LINK "")
if(CELOSIA_LTO)
    list(APPEND CELOSIA_OPT_COMPILE -flto -fno-fat-lto-objects)
    list(APPEND CELOSIA_LTO_COMPILE -flto -fno-fat-lto-objects)
    list(APPEND CELOSIA_OPT_LINK "-flto=${CELOSIA_LTO_JOBS}" -fuse-linker-plugin)
endif()
if(CELOSIA_PGO STREQUAL "GENERATE")
    if(NOT CELOSIA_PGO_DIR MATCHES "^sdmc:/")
        message(FATAL_ERROR "CELOSIA_PGO=GENERATE needs CELOSIA_PGO_DIR=sdmc:/...")
    endif()
    # -fno-profile-values: libgcov's TOPN (indirect-call) lists, updated concurrently by game threads,
    # made __gcov_dump loop forever on owned_backend.cpp.gcda on the console. Edge counters remain.
    list(APPEND CELOSIA_OPT_COMPILE "-fprofile-generate=${CELOSIA_PGO_DIR}" -fprofile-update=atomic -fno-profile-values
        "-fprofile-prefix-path=${celosia_profile_prefix}")
    list(APPEND CELOSIA_OPT_LINK "-fprofile-generate=${CELOSIA_PGO_DIR}" -fprofile-update=atomic)
elseif(CELOSIA_PGO STREQUAL "USE")
    if(NOT IS_DIRECTORY "${CELOSIA_PGO_DIR}")
        message(FATAL_ERROR "CELOSIA_PGO=USE needs CELOSIA_PGO_DIR pointing at the collected .gcda files")
    endif()
    file(TO_NATIVE_PATH "${CELOSIA_PGO_DIR}" celosia_profile_dir)
    # partial-training: code the shrine route never ran keeps normal -O3 optimisation instead of
    # being treated as cold, so untrained areas (colossus fights, menus) are not pessimised.
    # -fprofile-correction: dumps/resets race with game threads, so counters are not flow-consistent.
    list(APPEND CELOSIA_OPT_COMPILE "-fprofile-use=${celosia_profile_dir}" -fprofile-partial-training -fno-profile-values -fprofile-correction
        "-fprofile-prefix-path=${celosia_profile_prefix}" -Wno-missing-profile
        # Sources edited after training (renderer) drop their stale profile instead of failing.
        -Wno-error=coverage-mismatch)
    list(APPEND CELOSIA_OPT_LINK "-fprofile-use=${celosia_profile_dir}" -fprofile-partial-training -fprofile-correction)
elseif(NOT CELOSIA_PGO STREQUAL "OFF")
    message(FATAL_ERROR "CELOSIA_PGO must be OFF, GENERATE or USE")
endif()
message(STATUS "Celosia optimisation: LTO=${CELOSIA_LTO} PGO=${CELOSIA_PGO} scope=${CELOSIA_PGO_SCOPE} ${CELOSIA_PGO_DIR}")

function(celosia_optimize_compile target)
    set(options ${CELOSIA_OPT_COMPILE})
    if(target STREQUAL "celosia_game_aot" AND CELOSIA_PGO_SCOPE STREQUAL "runtime")
        set(options ${CELOSIA_LTO_COMPILE})
    endif()
    if(options)
        target_compile_options(${target} PRIVATE ${options})
    endif()
endfunction()
