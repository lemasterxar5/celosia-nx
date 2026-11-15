#include "native_aot_runtime.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace {
using Memory = celosia::native::DataMemory;
Memory &checkedMemory(uint8_t *ram, PS2Runtime *runtime)
{
    if (!runtime || ram != runtime->memory().mainRam().data())
        throw std::invalid_argument("Native libc: foreign data base");
    return runtime->memory();
}
void checkRange(const Memory &memory, uint32_t address, uint32_t size)
{
    if (!memory.contains(address,size)) throw std::out_of_range("Native libc: invalid data range");
}
std::optional<uint32_t> mainOffset(uint32_t address)
{
    for (const uint32_t base : {0u,0x20000000u,0x80000000u,0xA0000000u})
        if (address >= base && address - base < Memory::MainBytes) return address - base;
    return std::nullopt;
}
uint32_t canonical(uint32_t address)
{
    if (const auto offset = mainOffset(address)) return *offset;
    if (address >= 0xF0000000 && address - 0xF0000000 < Memory::ScratchBytes)
        return 0x70000000 + (address - 0xF0000000);
    return address;
}
void copyData(Memory &memory, uint32_t destination, uint32_t source, uint32_t bytes)
{
    if (!bytes) return;
    checkRange(memory,source,bytes); checkRange(memory,destination,bytes);
    const auto from = mainOffset(source), to = mainOffset(destination);
    if (from && to) {
        auto data = memory.mainRam();
        std::memmove(data.data()+*to,data.data()+*from,bytes);
        return;
    }
    std::array<uint8_t,4096> block;
    const uint32_t src = canonical(source), dst = canonical(destination);
    const bool backwards = dst > src && uint64_t(dst) < uint64_t(src)+bytes;
    for (uint32_t done = 0; done < bytes;) {
        const auto count = std::min<uint32_t>(block.size(),bytes-done);
        const uint32_t offset = backwards ? bytes-done-count : done;
        if (!memory.read(source+offset,block.data(),count) ||
            !memory.write(destination+offset,block.data(),count))
            throw std::runtime_error("Native libc: copy failed");
        done += count;
    }
}
uint8_t byteAt(const Memory &memory, uint32_t address)
{
    uint8_t value;
    if (!memory.read(address,&value,1)) throw std::out_of_range("Native libc: unterminated data string");
    return value;
}
uint32_t stringLength(const Memory &memory, uint32_t address)
{
    uint32_t length = 0;
    while (byteAt(memory,address+length)) ++length;
    return length;
}
int compareStrings(const Memory &memory, uint32_t a, uint32_t b, uint32_t limit)
{
    for (uint32_t i = 0; i < limit; ++i) {
        const auto left = byteAt(memory,a+i), right = byteAt(memory,b+i);
        if (left != right) return int(left)-int(right);
        if (!left) return 0;
    }
    return 0;
}
}
namespace ps2_stubs {
void memmove(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &memory = checkedMemory(ram,runtime);
    const auto destination = getRegU32(ctx,4);
    copyData(memory,destination,getRegU32(ctx,5),getRegU32(ctx,6));
    setReturnU32(ctx,destination);
}
void memcpy(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) { memmove(ram,ctx,runtime); }
void memset(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &memory = checkedMemory(ram,runtime);
    const auto destination = getRegU32(ctx,4), bytes = getRegU32(ctx,6);
    const auto value = static_cast<uint8_t>(getRegU32(ctx,5));
    if (bytes) {
        checkRange(memory,destination,bytes);
        if (const auto offset = mainOffset(destination))
            std::memset(memory.mainRam().data()+*offset,value,bytes);
        else {
            std::array<uint8_t,256> block; block.fill(value);
            for (uint32_t done = 0; done < bytes;) {
                const auto count = std::min<uint32_t>(block.size(),bytes-done);
                if (!memory.write(destination+done,block.data(),count))
                    throw std::runtime_error("Native libc: fill failed");
                done += count;
            }
        }
    }
    setReturnU32(ctx,destination);
}
void memcmp(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const auto &memory = checkedMemory(ram,runtime);
    const auto a = getRegU32(ctx,4), b = getRegU32(ctx,5), bytes = getRegU32(ctx,6);
    int result = 0;
    if (bytes) {
        checkRange(memory,a,bytes); checkRange(memory,b,bytes);
        std::array<uint8_t,256> left,right;
        for (uint32_t done = 0; done < bytes && !result;) {
            const auto count = std::min<uint32_t>(left.size(),bytes-done);
            if (!memory.read(a+done,left.data(),count) || !memory.read(b+done,right.data(),count))
                throw std::runtime_error("Native libc: compare failed");
            result = std::memcmp(left.data(),right.data(),count);
            done += count;
        }
    }
    setReturnS32(ctx,result);
}
void strlen(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,stringLength(checkedMemory(ram,runtime),getRegU32(ctx,4))); }
void strcpy(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &memory = checkedMemory(ram,runtime);
    const auto destination = getRegU32(ctx,4), source = getRegU32(ctx,5);
    copyData(memory,destination,source,stringLength(memory,source)+1);
    setReturnU32(ctx,destination);
}
void strncpy(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if(!ctx) throw std::invalid_argument("Native strncpy: null context");
    auto &memory=checkedMemory(ram,runtime);
    const auto destination=getRegU32(ctx,4),source=getRegU32(ctx,5),bytes=getRegU32(ctx,6);
    if(bytes) {
        checkRange(memory,destination,bytes);
        uint32_t copied=0;
        // Validate every byte that will be read before changing output. Padding
        // must not read beyond the source's first NUL or bounded data region.
        while(copied<bytes) {
            const auto at=uint64_t(source)+copied;
            if(at>UINT32_MAX) throw std::out_of_range("Native strncpy: source overflow");
            if(!byteAt(memory,static_cast<uint32_t>(at))) break;
            ++copied;
        }
        copyData(memory,destination,source,copied);
        if(copied<bytes && !memory.zero(destination+copied,bytes-copied))
            throw std::logic_error("Native strncpy: validated padding failed");
    }
    setReturnU32(ctx,destination);
}
void strcmp(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnS32(ctx,compareStrings(checkedMemory(ram,runtime),getRegU32(ctx,4),getRegU32(ctx,5),UINT32_MAX)); }
void strncmp(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnS32(ctx,compareStrings(checkedMemory(ram,runtime),getRegU32(ctx,4),getRegU32(ctx,5),getRegU32(ctx,6))); }
}
