#include "native_elf_data.hpp"
#include "native_memory.hpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace celosia::native;
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
static void u16(std::vector<uint8_t> &b,size_t p,uint16_t v) { b[p]=v; b[p+1]=v>>8; }
static void u32(std::vector<uint8_t> &b,size_t p,uint32_t v) { for(unsigned i=0;i<4;++i) b[p+i]=v>>(8*i); }
static std::vector<uint8_t> fixture()
{
    std::vector<uint8_t> b(0x208);
    b[0]=0x7F; b[1]='E'; b[2]='L'; b[3]='F'; b[4]=b[5]=b[6]=1;
    u16(b,16,2); u16(b,18,8); u32(b,20,1); u32(b,24,0x1000); u32(b,28,52);
    u16(b,40,52); u16(b,42,32); u16(b,44,2);
    for(unsigned i=0;i<2;++i) {
        const auto p=52+i*32; const auto address=0x1000+i*0x1000;
        u32(b,p,1); u32(b,p+4,0x100+i*0x100); u32(b,p+8,address); u32(b,p+12,address);
        u32(b,p+16,8); u32(b,p+20,24); u32(b,p+24,i?6:5); u32(b,p+28,0x100);
        for(unsigned n=0;n<8;++n) b[0x100+i*0x100+n]=uint8_t(1+i*8+n);
    }
    return b;
}
int main(int argc,char **argv)
{
    DataMemory memory;
    const auto valid=fixture();
    const auto result=loadElfData(memory,valid,0x1000);
    require(result.entry==0x1000 && result.loadSegments==2 && result.dataEnd==0x2018,"valid load metadata");
    std::array<uint8_t,24> data{};
    require(memory.read(0x1000,data.data(),data.size()) && data[0]==1 && data[7]==8 && data[8]==0 && data[23]==0,"file data and zero BSS");
    const std::array<uint8_t,16> guard{0xA5,0xA5,0xA5,0xA5};
    auto reject=[&](const std::vector<uint8_t> &bad) {
        memory.write(0x1000,guard.data(),guard.size()); bool failed=false;
        try { (void)loadElfData(memory,bad,0x1000); } catch(const std::invalid_argument &) { failed=true; }
        std::array<uint8_t,16> after{}; memory.read(0x1000,after.data(),after.size());
        require(failed && after==guard,"invalid ELF mutated data or was accepted");
    };
    for(const auto &change : std::vector<std::function<void(std::vector<uint8_t>&)>>{
        [](auto &b){b[0]=0;},[](auto &b){b[4]=2;},[](auto &b){b[5]=2;},[](auto &b){b[6]=2;},
        [](auto &b){u16(b,16,3);},[](auto &b){u16(b,18,62);},[](auto &b){u32(b,20,2);},
        [](auto &b){u16(b,40,51);},[](auto &b){u16(b,42,31);},[](auto &b){u16(b,44,0);},
        [](auto &b){u16(b,44,129);},[](auto &b){u32(b,28,UINT32_MAX);},
        [](auto &b){u32(b,24,0x1001);},[](auto &b){u32(b,24,0x1010);},
        [](auto &b){u32(b,24,0x1004);},
        [](auto &b){u32(b,52,3);},[](auto &b){u32(b,52+16,25);},
        [](auto &b){u32(b,52+28,3);},[](auto &b){u32(b,52+24,0x80);},
        [](auto &b){u32(b,84+8,0x1008);u32(b,84+12,0x1008);u32(b,84+28,1);},
        [](auto &b){u32(b,84+4,UINT32_MAX);},[](auto &b){u32(b,84+20,UINT32_MAX);},
        [](auto &b){u32(b,84+8,0x12000000);u32(b,84+12,0x12000000);},
        [](auto &b){u32(b,84+8,0x80002000);u32(b,84+12,0x80002000);},
        [](auto &b){u32(b,84+12,0x3000);},[](auto &b){u32(b,84+28,0x200);u32(b,84+4,0x208);}
    }) { auto bad=valid; change(bad); reject(bad); }
    reject({}); auto shortFile=valid; shortFile.resize(80); reject(shortFile);
    auto metadata=valid; metadata.resize(0x298); u16(metadata,44,3);
    u32(metadata,116,0x70000000); u32(metadata,120,0x280); u32(metadata,124,0x3000); u32(metadata,128,0x3000);
    u32(metadata,132,24); u32(metadata,136,24); u32(metadata,140,4); u32(metadata,144,4); u32(metadata,0x294,0x4000);
    require(loadElfData(memory,metadata,0x1000).gp==0x4000,"MIPS metadata is parsed as data only");
    auto badMeta=metadata; u32(badMeta,132,23); reject(badMeta);
    memory.write(0x8000,valid.data(),valid.size()); bool failed=false;
    try { (void)loadElfData(memory,std::span<const uint8_t>(memory.mainRam().data()+0x8000,valid.size()),0x1000); }
    catch(const std::invalid_argument &) { failed=true; }
    require(failed,"source alias into destination storage rejected");
    auto huge=std::make_unique_for_overwrite<uint8_t[]>(ElfDataMaxBytes+1);
    failed=false; try { (void)loadElfData(memory,{huge.get(),ElfDataMaxBytes+1}); } catch(const std::invalid_argument &) { failed=true; }
    require(failed,"file budget checked before reading payload");
    if(argc>1) {
        std::ifstream stream(argv[1],std::ios::binary|std::ios::ate);
        const auto length=static_cast<std::streamoff>(stream.tellg());
        if(!stream || length<0 || uint64_t(length)>ElfDataMaxBytes) throw std::runtime_error("ELF input size/open failure");
        stream.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if(!stream.read(reinterpret_cast<char *>(bytes.data()),length)) throw std::runtime_error("ELF input read failure");
        const auto actual=loadElfData(memory,bytes,0x100010);
        require(actual.loadSegments==2 && actual.gp==0x141D70 && actual.dataEnd==0x16A398,"private SCUS ELF data layout");
    }
    std::printf("native ELF data: %u checks passed\n",checks);
}
