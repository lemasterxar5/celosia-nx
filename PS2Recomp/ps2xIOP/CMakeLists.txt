cmake_minimum_required(VERSION 3.21)

project(ps2xIOP LANGUAGES CXX)

option(PS2X_IOP_BUILD_TESTS "Build ps2xIOP emulator smoke tests" OFF)
option(PS2X_IOP_BUILD_NATIVE_TESTS "Build standalone native sound service tests" OFF)
option(PS2X_IOP_NATIVE_ONLY "Build host services without an IOP CPU or peripheral emulator" OFF)
if(PS2X_IOP_NATIVE_ONLY AND PS2X_IOP_BUILD_TESTS)
    message(FATAL_ERROR "Interpreter tests and native-only services require separate build directories")
endif()

add_library(ps2_iop STATIC
    src/ps2_path.cpp
    src/iop_module_manager.cpp
    src/emulator/services/iop_sg2.cpp
    src/native/sound_mixer.cpp
    src/modules/dbcman.cpp
    src/modules/libsd.cpp
    src/modules/mcserv.cpp
)

if(PS2X_IOP_NATIVE_ONLY)
    target_sources(ps2_iop PRIVATE src/native/iop_subsystem.cpp src/native/audio_arena.cpp src/native/controller_service.cpp)
    target_compile_definitions(ps2_iop PUBLIC PS2X_IOP_NATIVE_ONLY=1)
else()
    target_sources(ps2_iop PRIVATE
    src/iop_subsystem.cpp
    src/emulator/iop_emulator.cpp
    src/emulator/core/iop_cpu.cpp
    src/emulator/core/iop_kernel.cpp
    src/emulator/core/iop_memory.cpp
    src/emulator/core/iop_spu2.cpp
    src/emulator/services/iop_module_loader.cpp
    src/emulator/services/iop_rpc.cpp
    src/emulator/imports/iop_cdvd.cpp
    src/emulator/imports/iop_heaplib.cpp
    src/emulator/imports/iop_imports.cpp
    src/emulator/imports/iop_intrman.cpp
    src/emulator/imports/iop_ioman.cpp
    src/emulator/imports/iop_loadcore.cpp
    src/emulator/imports/iop_stdio.cpp
    src/emulator/imports/iop_sysclib.cpp
    src/emulator/imports/iop_sysmem.cpp
    src/emulator/imports/iop_timrman.cpp
    src/emulator/imports/iop_vblank.cpp
    )
endif()

target_compile_features(ps2_iop PUBLIC cxx_std_20)

target_include_directories(ps2_iop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(ps2x::iop ALIAS ps2_iop)

if(PS2X_IOP_BUILD_TESTS)
    enable_testing()
    add_executable(ps2_iop_emulator_tests tests/iop_emulator_tests.cpp)
    target_link_libraries(ps2_iop_emulator_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_emulator_tests COMMAND ps2_iop_emulator_tests)

    add_executable(ps2_iop_import_tests tests/iop_import_tests.cpp)
    target_link_libraries(ps2_iop_import_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_tests COMMAND ps2_iop_import_tests)

    add_executable(ps2_iop_compatibility_tests tests/iop_compatibility_tests.cpp)
    target_link_libraries(ps2_iop_compatibility_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_compatibility_tests COMMAND ps2_iop_compatibility_tests)

    add_executable(ps2_iop_import_version_tests tests/iop_import_version_tests.cpp)
    target_link_libraries(ps2_iop_import_version_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_version_tests COMMAND ps2_iop_import_version_tests)

endif()

if(PS2X_IOP_BUILD_NATIVE_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(native_audio_arena_tests tests/native_audio_arena_tests.cpp src/native/audio_arena.cpp)
    add_executable(native_sg2_tests tests/native_sg2_tests.cpp src/native/audio_arena.cpp
        src/native/sound_mixer.cpp src/emulator/services/iop_sg2.cpp)
    add_executable(native_sg2_validation_tests tests/native_sg2_validation_tests.cpp src/native/audio_arena.cpp
        src/native/sound_mixer.cpp src/emulator/services/iop_sg2.cpp)
    target_compile_definitions(native_sg2_validation_tests PRIVATE PS2X_IOP_NATIVE_ONLY=1)
    foreach(native_test IN ITEMS native_audio_arena_tests native_sg2_tests native_sg2_validation_tests)
        target_compile_features(${native_test} PRIVATE cxx_std_20)
        target_include_directories(${native_test} PRIVATE include src)
        target_link_libraries(${native_test} PRIVATE Threads::Threads)
        add_test(NAME ${native_test} COMMAND ${native_test})
        if(PS2X_IOP_NATIVE_ONLY)
            target_compile_definitions(${native_test} PRIVATE PS2X_IOP_NATIVE_ONLY=1)
        endif()
    endforeach()
    if(PS2X_IOP_NATIVE_ONLY)
        add_executable(native_subsystem_tests tests/native_subsystem_tests.cpp)
        target_link_libraries(native_subsystem_tests PRIVATE ps2_iop Threads::Threads)
        add_test(NAME native_subsystem_tests COMMAND native_subsystem_tests)
    endif()
endif()

install(TARGETS ps2_iop
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
