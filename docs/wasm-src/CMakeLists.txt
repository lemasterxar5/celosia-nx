# lsfgprep.wasm: the installer's Lossless.dll preparation, built with Emscripten. It must give the same bytes as
# colosia-nx/private/lsfg/prep/lsfgprep.exe (the cache the game uses):
#   - lsfg-deko3d: the copy lsfgprep.exe uses (Cemu-nx 1.3.3 third_party; its shared files equal the game's dependencies/)
#   - version.hpp: the game's fixed strings
#   - uam: devkitPro uam v1.1.0, unpatched (the uam.exe that made the cache), driven as its command line tool does
#     (one DekoCompiler per shader, OutputDksh to a file). Its bison/flex/mako outputs are in uam-gen/, made from
#     uam 1.1.0's own grammars with the commands of its meson.build (gen-uam.bat: win_bison 3.8.2, win_flex 2.6.4, mako).
#   emcmake cmake -S wasm-src -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release ; cmake --build build-wasm
cmake_minimum_required(VERSION 3.20)
project(lsfgprep_wasm C CXX)

set(PRIV "${CMAKE_CURRENT_SOURCE_DIR}/../../colosia-nx/private/lsfg")
set(LSFG_DEKO3D_ROOT "${PRIV}/ref-Cemu-nx-1.3.3/third_party/lsfg-deko3d" CACHE PATH "")
set(SPIRV_CROSS_ROOT "${PRIV}/SPIRV-Cross" CACHE PATH "")
set(UAM_ROOT "C:/Users/emi/uam-1.1.0" CACHE PATH "")
set(UAM_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/uam-gen" CACHE PATH "")

# ---- uam 1.1.0 (meson.build, without main.cpp) ----
set(M "${UAM_ROOT}/mesa-imported")
set(UAM_SOURCES
  ${UAM_ROOT}/source/compiler_iface.cpp ${UAM_ROOT}/source/glsl_frontend.cpp
  ${UAM_ROOT}/source/mini-os.c ${UAM_ROOT}/source/tgsi_support.cpp
  ${UAM_GENERATED}/glsl/glsl_parser.cpp ${UAM_GENERATED}/glsl/glsl_lexer.cpp
  ${UAM_GENERATED}/glsl/glcpp/glcpp-parse.c ${UAM_GENERATED}/glsl/glcpp/glcpp-lex.c)
foreach(sub codegen tgsi util cso_cache glsl glsl/glcpp compiler program state_tracker main)
  file(READ ${M}/${sub}/meson.build _mb)
  string(REGEX MATCHALL "files\\(([^)]*)\\)" _blocks "${_mb}")
  foreach(_block IN LISTS _blocks)
    string(REGEX MATCHALL "'[^']+'" _names "${_block}")
    foreach(_n IN LISTS _names)
      string(REPLACE "'" "" _n "${_n}")
      list(APPEND UAM_SOURCES ${M}/${sub}/${_n})
    endforeach()
  endforeach()
endforeach()
list(REMOVE_DUPLICATES UAM_SOURCES)
add_library(uam110 STATIC ${UAM_SOURCES})
target_include_directories(uam110 PUBLIC ${UAM_ROOT}/source ${M}
  PRIVATE ${UAM_GENERATED} ${UAM_GENERATED}/glsl ${UAM_GENERATED}/glsl/glcpp ${M}/glsl ${M}/glsl/glcpp)
set_target_properties(uam110 PROPERTIES C_STANDARD 99 CXX_STANDARD 11)
target_compile_definitions(uam110 PRIVATE "PACKAGE_STRING=\"uam 1.1.0\"" DESKTOP _USE_MATH_DEFINES _GNU_SOURCE
                                          HAVE_POSIX_MEMALIGN NDEBUG
                                          )
target_compile_definitions(uam110 PUBLIC PIPE_ARCH_LITTLE_ENDIAN)  # wasm32: little-endian, unknown to p_config.h (also for users of its headers)
target_compile_options(uam110 PRIVATE -w -O3 -fexceptions)

# ---- SPIRV-Cross (the files lsfg-deko3d's CMake uses) ----
add_library(spirv_cross_min STATIC
  "${SPIRV_CROSS_ROOT}/spirv_cfg.cpp" "${SPIRV_CROSS_ROOT}/spirv_cross.cpp"
  "${SPIRV_CROSS_ROOT}/spirv_cross_parsed_ir.cpp" "${SPIRV_CROSS_ROOT}/spirv_glsl.cpp"
  "${SPIRV_CROSS_ROOT}/spirv_parser.cpp")
target_include_directories(spirv_cross_min SYSTEM PUBLIC "${SPIRV_CROSS_ROOT}")
target_compile_options(spirv_cross_min PRIVATE -w -O2 -fexceptions)

# ---- lsfg-deko3d + entry point ----
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gen/lsfg/common")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.hpp" "${CMAKE_CURRENT_BINARY_DIR}/gen/lsfg/common/version.hpp" COPYONLY)
set(L "${LSFG_DEKO3D_ROOT}/src")
add_executable(lsfgprep
  lsfgprep_wasm.cpp compile_uam_wasm.cpp
  ${L}/binding.cpp ${L}/cache_format.cpp ${L}/cache_load.cpp ${L}/cache_store.cpp ${L}/dksh.cpp ${L}/error.cpp
  ${L}/image_graph.cpp ${L}/layout.cpp ${L}/pe_resources.cpp ${L}/prepare.cpp ${L}/schedule.cpp ${L}/sha256.cpp
  ${L}/shader_set.cpp ${L}/spirv_module.cpp ${L}/translate.cpp)
set_target_properties(lsfgprep PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON SUFFIX ".js")
target_include_directories(lsfgprep PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen" "${LSFG_DEKO3D_ROOT}/include")
target_compile_options(lsfgprep PRIVATE -O2 -fexceptions)
target_link_libraries(lsfgprep PRIVATE uam110 spirv_cross_min)
# LSFGPREP_NODE=ON: a node build for the byte comparison with lsfgprep.exe (wasm-src/compare.js)
option(LSFGPREP_NODE "build for node (test)" OFF)
if(LSFGPREP_NODE)
  set(_env "node" )
  set(_extra)
else()
  set(_env "worker")
  set(_extra)
endif()
target_link_options(lsfgprep PRIVATE
  -O2 -fexceptions ${_extra}
  -sMODULARIZE=1 -sEXPORT_NAME=createLsfgPrep -sENVIRONMENT=${_env}
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=8388608
  -sFORCE_FILESYSTEM=1 "-sEXPORTED_RUNTIME_METHODS=['ccall','FS','UTF8ToString']"
  "-sINCOMING_MODULE_JS_API=['wasmBinary','print','printErr']"  # the page passes the .wasm bytes (works from file://)
  "-sEXPORTED_FUNCTIONS=['_lsfgprep_run','_lsfgprep_error','_malloc','_free']")
