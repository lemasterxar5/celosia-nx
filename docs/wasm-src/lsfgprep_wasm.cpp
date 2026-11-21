// lsfgprep para WebAssembly: Lossless.dll -> caché de shaders de lsfg-deko3d (manifest.bin + NN.dksh),
// lo mismo que PrepareShaders de Cemu-nx 1.3.3 (DKFrameGenerationShaders.cpp) y private/lsfg/prep/main.cpp,
// pero en el navegador. El JS escribe el DLL en el sistema de archivos en memoria de Emscripten, llama a
// lsfgprep_run y lee el directorio de salida.
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/error.hpp>
#include <lsfg/common/prepare.hpp>

#include <emscripten/emscripten.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::string g_error;

extern "C" EMSCRIPTEN_KEEPALIVE const char *lsfgprep_error() { return g_error.c_str(); }

extern "C" EMSCRIPTEN_KEEPALIVE int lsfgprep_run(const char *dllPath, const char *outDir, int performance) {
    g_error.clear();
    std::ifstream f(dllPath, std::ios::binary);
    std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(f)), {});
    if (image.empty() || image.size() > 512u * 1024u * 1024u) { g_error = "Lossless.dll could not be read"; return 1; }
    const lsfg::prepare::Options options{
        .precision = lsfg::shaders::Precision::high,
        .graph = {.performance = performance != 0},
    };
    lsfg::prepare::Result result;
    const auto prepared = lsfg::prepare::run(image, options, result);
    if (!lsfg::succeeded(prepared)) {
        g_error = "The shaders could not be prepared (" + std::string(lsfg::error_name(prepared)) + ")";
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (!lsfg::succeeded(lsfg::cache::write(outDir, result.contents))) { g_error = "The cache could not be written"; return 3; }
    lsfg::cache::Loaded back;
    if (!lsfg::succeeded(lsfg::cache::read(outDir, back)) || !lsfg::cache::same_modules(back, result.contents)) {
        g_error = "The written cache does not match when read back"; return 4;
    }
    return 0;
}
