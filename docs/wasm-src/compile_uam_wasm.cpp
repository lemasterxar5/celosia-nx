// lsfg::dksh::compile for WebAssembly: compute GLSL -> DKSH with devkitPro uam 1.1.0, unpatched, the way its
// command line tool does it (source/main.cpp: one DekoCompiler, CompileGlsl, OutputDksh to a file), which is what
// lsfgprep.exe ran (private/lsfg/prep/compile_uam_pc.cpp: uam.exe -s comp). The file lives in Emscripten's
// in-memory file system and is read back.
#include <lsfg/common/dksh.hpp>

#include "compiler_iface.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace lsfg::dksh {

bool compiler_available() noexcept { return true; }

ErrorCode compile(const std::string_view glsl, Blob &out) {
    out = Blob{};
    if (glsl.empty()) return ErrorCode::invalid_argument;
    const std::string source(glsl);
    const char *path = "/tmp/lsfgprep_uam.dksh";
    std::remove(path);
    {
        DekoCompiler compiler{pipeline_stage_compute};
        if (!compiler.CompileGlsl(source.c_str())) {
            out.log = "uam: compile failed";
            return ErrorCode::shader_compile_failed;
        }
        compiler.OutputDksh(path);
    }
    std::ifstream r(path, std::ios::binary);
    if (!r) { out.log = "uam: no output"; return ErrorCode::shader_compile_failed; }
    out.bytes.assign(std::istreambuf_iterator<char>(r), {});
    r.close();
    std::remove(path);
    return validate(out.bytes, out.program);
}

}  // namespace lsfg::dksh
