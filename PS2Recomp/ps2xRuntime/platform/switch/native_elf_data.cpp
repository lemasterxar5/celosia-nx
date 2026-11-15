#include "native_elf_data.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>
namespace celosia::native {
namespace {
[[noreturn]] void invalid(const char *reason) { throw std::invalid_argument(reason); }
uint16_t u16(std::span<const uint8_t> b,size_t p) { return uint16_t(b[p])|(uint16_t(b[p+1])<<8); }
uint32_t u32(std::span<const uint8_t> b,size_t p)
{
    uint32_t value=0;
    for(unsigned i=0;i<4;++i) value|=uint32_t(b[p+i])<<(i*8);
    return value;
}
bool inFile(size_t length,uint32_t offset,uint32_t bytes)
{ return offset<=length && bytes<=length-offset; }
struct Segment { uint32_t offset,address,fileBytes,memoryBytes,flags; };
struct Header { uint32_t entry,table; uint16_t count; };
Header validateHeader(const DataMemory &memory,std::span<const uint8_t> b,uint32_t expectedEntry)
{
    if(b.size()<52 || b.size()>ElfDataMaxBytes || !b.data()) invalid("Native ELF: file size");
    const auto input=reinterpret_cast<uintptr_t>(b.data());
    const auto main=memory.mainRam();
    const auto destination=reinterpret_cast<uintptr_t>(main.data());
    if(input>std::numeric_limits<uintptr_t>::max()-b.size() ||
       (input<destination+main.size() && destination<input+b.size())) invalid("Native ELF: aliased source");
    if(b[0]!=0x7F || b[1]!='E' || b[2]!='L' || b[3]!='F' || b[4]!=1 || b[5]!=1 || b[6]!=1 ||
       b[7]!=0 || b[8]!=0 || u16(b,16)!=2 || u16(b,18)!=8 || u32(b,20)!=1 || u16(b,40)!=52)
        invalid("Native ELF: unsupported executable header");
    const auto entry=u32(b,24), table=u32(b,28);
    const auto count=u16(b,44);
    if(u16(b,42)!=32 || !count || count>128 || table<52 ||
       !inFile(b.size(),table,uint32_t(count)*32)) invalid("Native ELF: program header table");
    if((entry&3) || (expectedEntry && entry!=expectedEntry)) invalid("Native ELF: entry mismatch");
    return {entry,table,count};
}
}
ElfDataImage loadElfData(DataMemory &memory,std::span<const uint8_t> b,uint32_t expectedEntry)
{
    const auto [entry,table,count]=validateHeader(memory,b,expectedEntry);
    std::vector<Segment> segments;
    segments.reserve(count);
    bool hasMetadata=false, entryBacked=false;
    uint32_t gp=0,end=0;
    for(unsigned i=0;i<count;++i) {
        const size_t p=table+i*32;
        const auto type=u32(b,p);
        if(!type) continue;
        const Segment s{u32(b,p+4),u32(b,p+8),u32(b,p+16),u32(b,p+20),u32(b,p+24)};
        const auto physical=u32(b,p+12), alignment=u32(b,p+28);
        if((type!=1 && type!=0x70000000) || s.address!=physical || (s.flags&~7u) || !(s.flags&4) ||
           !s.memoryBytes || s.fileBytes>s.memoryBytes || !inFile(b.size(),s.offset,s.fileBytes) ||
           s.address>=DataMemory::MainBytes || s.memoryBytes>DataMemory::MainBytes-s.address ||
           (alignment>1 && (alignment>DataMemory::MainBytes || (alignment&(alignment-1)) ||
                            ((s.offset^s.address)&(alignment-1)))))
            invalid("Native ELF: unsupported or out-of-range segment");
        if(type==0x70000000) {
            if(hasMetadata || s.fileBytes!=24 || s.memoryBytes!=24 || s.flags!=4)
                invalid("Native ELF: MIPS metadata");
            hasMetadata=true;
            gp=u32(b,s.offset+20);
            if(gp>=DataMemory::MainBytes) invalid("Native ELF: global data pointer");
            continue;
        }
        for(const auto &prior:segments)
            if(s.address<uint64_t(prior.address)+prior.memoryBytes && prior.address<uint64_t(s.address)+s.memoryBytes)
                invalid("Native ELF: overlapping data destinations");
        if((s.flags&1) && entry>=s.address && uint64_t(entry)+4<=uint64_t(s.address)+s.fileBytes)
            entryBacked=true;
        end=std::max(end,s.address+s.memoryBytes);
        segments.push_back(s);
    }
    if(!entryBacked) invalid("Native ELF: entry is not file-backed executable data");
    // No allocations or further input validation after this point. DataMemory
    // is final/stable and every destination has already passed its main bounds.
    for(const auto &s:segments) {
        if(s.fileBytes && !memory.write(s.address,b.data()+s.offset,s.fileBytes))
            throw std::logic_error("Native ELF: validated data copy failed");
        if(!memory.zero(s.address+s.fileBytes,s.memoryBytes-s.fileBytes))
            throw std::logic_error("Native ELF: validated BSS clear failed");
    }
    return {entry,gp,end,static_cast<uint32_t>(segments.size())};
}
}
