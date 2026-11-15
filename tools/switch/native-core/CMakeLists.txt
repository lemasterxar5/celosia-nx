cmake_minimum_required(VERSION 3.21)
project(CelosiaNativeCore LANGUAGES CXX)
option(CELOSIA_NATIVE_CORE_TESTS "Build native core host tests" ON)
get_filename_component(CELOSIA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(CELOSIA_PLATFORM "${CELOSIA_ROOT}/PS2Recomp/ps2xRuntime/platform/switch")
set(PS2X_IOP_NATIVE_ONLY ON CACHE BOOL "CPU-free native services only" FORCE)
add_subdirectory("${CELOSIA_ROOT}/PS2Recomp/ps2xIOP" native-services)
# Explicit sources: never inherit the original device/runtime target.
add_library(celosia_native_data STATIC "${CELOSIA_PLATFORM}/native_memory.cpp")
target_sources(celosia_native_data PRIVATE "${CELOSIA_PLATFORM}/native_elf_data.cpp" "${CELOSIA_PLATFORM}/native_files.cpp"
    "${CELOSIA_PLATFORM}/native_assets.cpp" "${CELOSIA_PLATFORM}/native_file_api.cpp"
    "${CELOSIA_PLATFORM}/native_save_store.cpp")
target_compile_features(celosia_native_data PUBLIC cxx_std_20)
target_include_directories(celosia_native_data PUBLIC "${CELOSIA_PLATFORM}")
add_library(celosia_native_aot STATIC "${CELOSIA_PLATFORM}/native_aot_runtime.cpp"
    "${CELOSIA_PLATFORM}/native_aot_memory.cpp" "${CELOSIA_PLATFORM}/native_libc.cpp"
    "${CELOSIA_PLATFORM}/native_heap.cpp" "${CELOSIA_PLATFORM}/native_boot_services.cpp"
    "${CELOSIA_PLATFORM}/native_alloc.cpp" "${CELOSIA_PLATFORM}/native_semaphore.cpp"
    "${CELOSIA_PLATFORM}/native_sync.cpp" "${CELOSIA_PLATFORM}/native_clock.cpp"
    "${CELOSIA_PLATFORM}/native_library.cpp" "${CELOSIA_PLATFORM}/native_cache.cpp"
    "${CELOSIA_PLATFORM}/native_printf.cpp" "${CELOSIA_PLATFORM}/native_threads.cpp"
    "${CELOSIA_PLATFORM}/native_thread_calls.cpp" "${CELOSIA_PLATFORM}/native_iop.cpp"
    "${CELOSIA_PLATFORM}/native_service_calls.cpp" "${CELOSIA_PLATFORM}/native_asset_calls.cpp"
    "${CELOSIA_PLATFORM}/native_file_calls.cpp" "${CELOSIA_PLATFORM}/native_input.cpp"
    "${CELOSIA_PLATFORM}/native_pad_api.cpp" "${CELOSIA_PLATFORM}/native_pad_calls.cpp"
    "${CELOSIA_PLATFORM}/native_faults.cpp" "${CELOSIA_PLATFORM}/native_platform_init.cpp"
    "${CELOSIA_PLATFORM}/native_kernel_publication.cpp" "${CELOSIA_PLATFORM}/native_kernel_services.cpp"
    "${CELOSIA_PLATFORM}/native_math_init.cpp" "${CELOSIA_PLATFORM}/native_executor.cpp"
    "${CELOSIA_PLATFORM}/native_module_image.cpp" "${CELOSIA_PLATFORM}/native_sg2_control.cpp"
    "${CELOSIA_PLATFORM}/native_game_publication.cpp" "${CELOSIA_PLATFORM}/native_save_control.cpp")
target_link_libraries(celosia_native_aot PUBLIC celosia_native_data ps2x::iop)
target_include_directories(celosia_native_aot PUBLIC "${CELOSIA_ROOT}/PS2Recomp/ps2xRuntime/include")
target_compile_definitions(celosia_native_aot PUBLIC PS2X_SWITCH_NATIVE_ONLY=1
    PS2X_GUEST_READ_WATCH=0 PS2X_GUEST_WRITE_TRACE=0)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CELOSIA_SDK_DEFAULT "$ENV{DEVKITPRO}")
    if(NOT EXISTS "${CELOSIA_SDK_DEFAULT}/libnx/include/switch.h")
        get_filename_component(CELOSIA_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(CELOSIA_SDK_DEFAULT "${CELOSIA_COMPILER_DIR}/../.." ABSOLUTE)
    endif()
    set(CELOSIA_DEVKITPRO "${CELOSIA_SDK_DEFAULT}" CACHE PATH "Installed devkitPro root")
    if(NOT EXISTS "${CELOSIA_DEVKITPRO}/libnx/include/switch.h")
        message(FATAL_ERROR "Set CELOSIA_DEVKITPRO to the installed SDK root")
    endif()
    target_compile_definitions(celosia_native_aot PUBLIC __SWITCH__)
    target_compile_definitions(celosia_native_data PUBLIC __SWITCH__)
    target_include_directories(celosia_native_aot SYSTEM PRIVATE "${CELOSIA_DEVKITPRO}/libnx/include")
    add_library(celosia_native_audio STATIC "${CELOSIA_PLATFORM}/native_audio_output.cpp")
    target_compile_features(celosia_native_audio PUBLIC cxx_std_20)
    target_compile_definitions(celosia_native_audio PRIVATE __SWITCH__)
    target_include_directories(celosia_native_audio PUBLIC "${CELOSIA_PLATFORM}")
    target_include_directories(celosia_native_audio SYSTEM PRIVATE "${CELOSIA_DEVKITPRO}/libnx/include")
    set(CELOSIA_SSE2NEON_DIR "" CACHE PATH "Pinned sse2neon v1.9.1 header directory")
    if(NOT EXISTS "${CELOSIA_SSE2NEON_DIR}/sse2neon.h")
        message(FATAL_ERROR "Native AOT ABI requires pinned sse2neon on ARM64")
    endif()
    target_compile_definitions(celosia_native_aot PUBLIC USE_SSE2NEON)
    target_include_directories(celosia_native_aot PUBLIC "${CELOSIA_SSE2NEON_DIR}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_SYSTEM_PROCESSOR MATCHES "(AMD64|x86_64)")
    target_compile_options(celosia_native_aot PUBLIC -march=x86-64-v2)
endif()
if(CELOSIA_NATIVE_CORE_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(native_memory_test "${CELOSIA_PLATFORM}/tests/native_memory_test.cpp")
    target_link_libraries(native_memory_test PRIVATE celosia_native_data)
    add_test(NAME native_memory_test COMMAND native_memory_test)
    add_executable(native_elf_data_test "${CELOSIA_PLATFORM}/tests/native_elf_data_test.cpp")
    target_link_libraries(native_elf_data_test PRIVATE celosia_native_data)
    add_test(NAME native_elf_data_test COMMAND native_elf_data_test)
    add_executable(native_audio_policy_test "${CELOSIA_PLATFORM}/tests/native_audio_policy_test.cpp")
    target_link_libraries(native_audio_policy_test PRIVATE celosia_native_data)
    add_test(NAME native_audio_policy_test COMMAND native_audio_policy_test)
    add_executable(native_aot_test "${CELOSIA_PLATFORM}/tests/native_aot_test.cpp")
    target_link_libraries(native_aot_test PRIVATE celosia_native_aot)
    add_test(NAME native_aot_test COMMAND native_aot_test)
    add_executable(native_libc_test "${CELOSIA_PLATFORM}/tests/native_libc_test.cpp")
    target_link_libraries(native_libc_test PRIVATE celosia_native_aot)
    target_include_directories(native_libc_test PRIVATE "${CELOSIA_ROOT}/PS2Recomp/ps2xRuntime/src/lib/Kernel")
    add_test(NAME native_libc_test COMMAND native_libc_test)
    add_executable(native_heap_test "${CELOSIA_PLATFORM}/tests/native_heap_test.cpp")
    target_link_libraries(native_heap_test PRIVATE celosia_native_aot Threads::Threads)
    add_test(NAME native_heap_test COMMAND native_heap_test)
    add_executable(native_boot_test "${CELOSIA_PLATFORM}/tests/native_boot_test.cpp")
    target_link_libraries(native_boot_test PRIVATE celosia_native_aot)
    target_include_directories(native_boot_test PRIVATE "${CELOSIA_ROOT}/PS2Recomp/ps2xRuntime/src/lib/Kernel")
    add_test(NAME native_boot_test COMMAND native_boot_test)
    foreach(test_name IN ITEMS
        native_semaphore_test native_sync_test native_clock_test native_library_test
        native_execution_test native_printf_test native_threads_test native_thread_calls_test
        native_iop_test native_files_test native_service_calls_test native_assets_test
        native_asset_calls_test native_file_api_test native_file_calls_test native_input_test
        native_pad_api_test native_pad_calls_test native_faults_test native_platform_init_test
        native_kernel_publication_test native_kernel_services_test native_math_init_test native_executor_test
        native_module_image_test native_sg2_batch_test native_sg2_control_test native_game_publication_test
        native_save_store_test native_save_control_test)
        add_executable(${test_name} "${CELOSIA_PLATFORM}/tests/${test_name}.cpp")
        target_link_libraries(${test_name} PRIVATE celosia_native_aot Threads::Threads)
        target_include_directories(${test_name} PRIVATE "${CELOSIA_ROOT}/PS2Recomp/ps2xRuntime/src/lib/Kernel")
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 15)
    endforeach()
    target_include_directories(native_sg2_batch_test PRIVATE "${CELOSIA_ROOT}/PS2Recomp/ps2xIOP/src")
    add_executable(native_executor_wait_failure_test "${CELOSIA_PLATFORM}/tests/native_executor_test.cpp"
        "${CELOSIA_PLATFORM}/native_executor.cpp")
    target_compile_definitions(native_executor_wait_failure_test PRIVATE CELOSIA_EXECUTOR_TEST_WAIT_FAILURE=1)
    target_link_libraries(native_executor_wait_failure_test PRIVATE celosia_native_aot Threads::Threads)
    add_test(NAME native_executor_wait_failure_test COMMAND native_executor_wait_failure_test)
    set_tests_properties(native_executor_wait_failure_test PROPERTIES TIMEOUT 15)
endif()
