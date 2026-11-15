#include "native_kernel_publication.hpp"
#include "native_aot_context.hpp"
#include "native_files.hpp"
#include "native_memory.hpp"
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace celosia::native {
namespace {
constexpr uint32_t Header=0x1AA7C0,Table=0x1AA870,Text=0x1AC998,TextBytes=0x341A4;
constexpr uint32_t FileBytes=415908,Entry=0x1B32A0,AllocationEnd=0x290F2C;
constexpr std::array<uint32_t,28> HeaderWords{
    0x32666678,0,0,0xF,Entry,0x658A4,0x210064,0,0x1AC904,0x77B,0x1E5200,0x1EC9B0,
    Table,0x1AAA70,6,0x1AC85C,16,0x1AA830,0x1AC904,0x6908,0x2144,0x3AA40,0x421F0,0xB0,0x2B0,0x209C,0x70,0x2144};
struct Section {uint32_t index;const char *name;std::array<uint32_t,8> words;};
constexpr std::array<Section,4> Sections{{
    {1,".text",{Text,Text,TextBytes,8,1,0,0,0x21D8}},
    {5,".data",{0x1E0B40,0x1E0B40,0xF9C,0x10,1,0,0,0x36380}},
    {8,".rodata",{0x1E1AE0,0x1E1AE0,0x371F,0x10,1,0,0,0x37320}},
    {11,".bss",{0x1F4800,Header,0x9C72C,0x40,8,1,2,0}}}};
constexpr std::array<uint8_t,32> RawDigest{
    0xCB,0xC5,0x89,0x29,0xD8,0xD4,0x8F,0xA3,0x96,0x8D,0xF2,0x5A,0x2E,0x80,0xA6,0xA7,
    0x30,0xD4,0xCD,0x61,0x10,0x7C,0x15,0x36,0x3D,0x6E,0x43,0x5C,0x96,0x81,0xE6,0x04};
constexpr std::array<uint8_t,32> TextDigest{
    0x09,0xC8,0x40,0xF2,0x30,0x4D,0xFF,0x3A,0x73,0x95,0x3B,0xD8,0x8B,0x98,0xC7,0xC6,
    0x37,0xDB,0x73,0xFD,0x5E,0xA5,0xE2,0x25,0xA1,0xAE,0xDC,0x3F,0x4F,0xF8,0x43,0xE9};
[[noreturn]] void mismatch(const char *reason) {throw std::runtime_error(reason);}
void expectValue(const char *name,uint32_t offset,uint32_t address,uint32_t actual,uint32_t expected)
{
    if(actual==expected) return;
    char message[192];
    std::snprintf(message,sizeof(message),
        "Kernel publication: %s offset=0x%03x address=0x%08x actual=0x%08x expected=0x%08x",
        name,offset,address,actual,expected);
    mismatch(message);
}
uint32_t word(const DataMemory &memory,uint32_t address)
{
    std::array<uint8_t,4> bytes{};
    if(!memory.read(address,bytes.data(),bytes.size())) mismatch("Kernel publication: metadata outside data bounds");
    return uint32_t(bytes[0]) | (uint32_t(bytes[1])<<8) | (uint32_t(bytes[2])<<16) | (uint32_t(bytes[3])<<24);
}
void expectWord(const DataMemory &memory,const char *name,uint32_t base,uint32_t offset,uint32_t expected)
{expectValue(name,offset,base+offset,word(memory,base+offset),expected);}
void validateContext(const DataMemory &memory,const R5900Context &context)
{
    expectValue("register RA",31,0,getRegU32(&context,31),0x101D3C);
    expectValue("register entry",17,0,getRegU32(&context,17),Entry);
    expectValue("register GP",28,0,getRegU32(&context,28),0x141D70);
    expectValue("register mode",4,0,getRegU32(&context,4),2);
    expectWord(memory,"startup control",0x131C00,0x44,Header);
    expectWord(memory,"allocation end",0x139E00,0,AllocationEnd);
    expectWord(memory,"allocation cursor",0x139E00,4,AllocationEnd);
}
void validateLayout(const DataMemory &memory)
{
    if(!memory.contains(Header,FileBytes) || !memory.contains(Table,16*32))
        mismatch("Kernel publication: loaded header/table outside data bounds");
    for(size_t i=0;i<HeaderWords.size();++i)
        expectWord(memory,"header",Header,uint32_t(i)*4,HeaderWords[i]);
    for(const auto &section:Sections) {
        for(size_t i=0;i<section.words.size();++i)
            expectWord(memory,section.name,Table+section.index*32,uint32_t(i)*4,section.words[i]);
        if(!memory.contains(section.words[0],section.words[2]))
            mismatch("Kernel publication: allocated section outside data bounds");
    }
}
struct OwnedFile {
    NativeFiles &files;
    uint64_t token;
    ~OwnedFile()
    {
        if(!token) return;
        try {if(!files.close(token)) std::fputs("Kernel publication: file close failed\n",stderr);}
        catch(...) {std::fputs("Kernel publication: file close threw\n",stderr);}
    }
    void close()
    {
        const auto previous=std::exchange(token,0);
        if(!files.close(previous)) mismatch("Kernel publication: native file close failed");
    }
};
std::vector<uint8_t> readKernel(NativeFiles &files)
{
    OwnedFile file{files,files.open("KERNEL.XFF")};
    if(!file.token) mismatch("Kernel publication: native KERNEL.XFF unavailable");
    if(files.size(file.token)!=FileBytes) mismatch("Kernel publication: native file size mismatch");
    std::vector<uint8_t> bytes(FileBytes);
    if(files.read(file.token,0,bytes)!=FileBytes || files.size(file.token)!=FileBytes)
        mismatch("Kernel publication: native file read or size changed");
    file.close();
    return bytes;
}
}
void validateKernelPublication(DataMemory &memory,const R5900Context &context,NativeFiles &files,Hasher hasher)
{
    if(!hasher) mismatch("Kernel publication: SHA-256 provider missing");
    validateContext(memory,context);
    validateLayout(memory);
    const auto source=readKernel(files);
    if(hasher(source)!=RawDigest) mismatch("Kernel publication: native source SHA-256 mismatch");
    // This range is static relocated code DATA matching the AOT compiler's
    // input section. Native execution still calls already compiled ARM code.
    const auto data=static_cast<const DataMemory &>(memory).mainRam();
    if(hasher(data.subspan(Text,TextBytes))!=TextDigest)
        mismatch("Kernel publication: loaded static text SHA-256 differs from AOT input");
}
}
