#include "native_aot_runtime.hpp"
#include "ps2_stubs.h"
#include <array>
#include <cstdio>
#include <stdexcept>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
static unsigned checks = 0;
static void require(bool ok, const char *why) { ++checks; if (!ok) throw std::runtime_error(why); }
int main()
{
    PS2Runtime runtime;
    R5900Context ctx;
    auto *ram = runtime.memory().mainRam().data();
    auto args = [&](uint32_t a, uint32_t b = 0, uint32_t c = 0) {
        ctx.r[4] = _mm_set_epi32(0,0,0,a); ctx.r[5] = _mm_set_epi32(0,0,0,b);
        ctx.r[6] = _mm_set_epi32(0,0,0,c);
    };
    const char text[] = "native";
    runtime.memory().write(0x100,text,sizeof(text));
    args(0x200,0x80000100,sizeof(text)); ps2_stubs::memcpy(ram,&ctx,&runtime);
    require(getRegU32(&ctx,2) == 0x200 && std::memcmp(ram+0x200,text,sizeof(text)) == 0, "native memcpy and alias");
    args(0x100); ps2_stubs::strlen(ram,&ctx,&runtime); require(getRegU32(&ctx,2) == 6, "native strlen");
    args(0x300,0x100); ps2_stubs::strcpy(ram,&ctx,&runtime);
    require(getRegU32(&ctx,2) == 0x300 && std::memcmp(ram+0x300,text,sizeof(text)) == 0, "native strcpy");
    args(0x100,0x300); ps2_stubs::strcmp(ram,&ctx,&runtime); require(getRegU32(&ctx,2) == 0, "native strcmp equal");
    ram[0x300] = 'o'; args(0x100,0x300); ps2_stubs::strcmp(ram,&ctx,&runtime);
    require(static_cast<int32_t>(getRegU32(&ctx,2)) < 0, "native strcmp ordering");
    args(0x100,0x300,0); ps2_stubs::strncmp(ram,&ctx,&runtime); require(getRegU32(&ctx,2) == 0, "zero comparison");
    args(0x300,0x123,4); ps2_stubs::memset(ram,&ctx,&runtime);
    require(ram[0x300] == 0x23 && ram[0x303] == 0x23, "native byte fill");
    args(0x100,0x200,sizeof(text)); ps2_stubs::memcmp(ram,&ctx,&runtime); require(getRegU32(&ctx,2) == 0, "native memcmp");
    std::array<uint8_t,8192> expected{}, output{};
    for (size_t i=0;i<expected.size();++i) expected[i] = static_cast<uint8_t>(i%251);
    runtime.memory().write(0x70000000,expected.data(),expected.size());
    std::memmove(expected.data()+512,expected.data(),6000);
    args(0xF0000200,0x70000000,6000); ps2_stubs::memmove(ram,&ctx,&runtime);
    require(runtime.memory().read(0x70000000,output.data(),output.size()) && output == expected,
            "overlapping scratch aliases copy backwards across chunk boundaries");
    args(0x100,0x12000000,4); bool rejected = false;
    try { ps2_stubs::memcpy(ram,&ctx,&runtime); } catch(const std::out_of_range &) { rejected = true; }
    require(rejected && std::memcmp(ram+0x100,text,sizeof(text)) == 0, "invalid source rejected before destination mutation");
    args(0x100,0,UINT32_MAX); rejected = false;
    try { ps2_stubs::memset(ram,&ctx,&runtime); } catch(const std::out_of_range &) { rejected = true; }
    require(rejected, "unbounded fill rejected");
    const char notTerminated = 'x'; runtime.memory().write(0x70003FFF,&notTerminated,1);
    args(0x70003FFF); rejected = false;
    try { ps2_stubs::strlen(ram,&ctx,&runtime); } catch(const std::out_of_range &) { rejected = true; }
    require(rejected, "unterminated string cannot cross buffer boundary");
    std::memset(ram+0x400,0xA5,16);
    args(0x400,0x80000100,10); ps2_stubs::strncpy(ram,&ctx,&runtime);
    require(getRegU32(&ctx,2)==0x400 && std::memcmp(ram+0x400,text,6)==0 &&
            ram[0x406]==0 && ram[0x409]==0 && ram[0x40A]==0xA5,"strncpy copies and zero pads exactly n");
    args(0x400,0x100,3); ps2_stubs::strncpy(ram,&ctx,&runtime);
    require(std::memcmp(ram+0x400,"nat",3)==0 && ram[0x403]=='i',"strncpy truncates without appending a NUL");
    args(0x1FFFFFF,0x100,2); rejected=false;
    ram[0x1FFFFFF]=0xA5;
    try { ps2_stubs::strncpy(ram,&ctx,&runtime); } catch(const std::out_of_range &) { rejected=true; }
    require(rejected && ram[0x1FFFFFF]==0xA5,"strncpy whole destination validated before mutation");
    args(0x400,0x70003FFF,2); rejected=false;
    try { ps2_stubs::strncpy(ram,&ctx,&runtime); } catch(const std::out_of_range &) { rejected=true; }
    require(rejected && ram[0x400]=='n',"strncpy whole source validated before mutation");
    args(0x12000000,0x12000000,0); ps2_stubs::strncpy(ram,&ctx,&runtime);
    require(getRegU32(&ctx,2)==0x12000000,"zero strncpy does not dereference pointers");
    std::printf("native libc: %u checks passed\n", checks);
}
