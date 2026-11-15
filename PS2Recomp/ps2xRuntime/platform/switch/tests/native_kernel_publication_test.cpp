#include "native_kernel_publication.hpp"
#include "native_aot_context.hpp"
#include "native_files.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    using namespace celosia::native;
    constexpr uint32_t Header=0x1AA7C0,Table=0x1AA870,Text=0x1AC998,TextBytes=0x341A4,FileBytes=415908;
    const std::array<uint32_t,28> header{
        0x32666678,0,0,0xF,0x1B32A0,0x658A4,0x210064,0,0x1AC904,0x77B,0x1E5200,0x1EC9B0,
        Table,0x1AAA70,6,0x1AC85C,16,0x1AA830,0x1AC904,0x6908,0x2144,0x3AA40,0x421F0,0xB0,0x2B0,0x209C,0x70,0x2144};
    struct Section {uint32_t index;std::array<uint32_t,8> words;};
    const std::array<Section,4> sections{{
        {1,{Text,Text,TextBytes,8,1,0,0,0x21D8}},
        {5,{0x1E0B40,0x1E0B40,0xF9C,0x10,1,0,0,0x36380}},
        {8,{0x1E1AE0,0x1E1AE0,0x371F,0x10,1,0,0,0x37320}},
        {11,{0x1F4800,Header,0x9C72C,0x40,8,1,2,0}}}};
    const std::array<uint8_t,32> rawDigest{
        0xCB,0xC5,0x89,0x29,0xD8,0xD4,0x8F,0xA3,0x96,0x8D,0xF2,0x5A,0x2E,0x80,0xA6,0xA7,
        0x30,0xD4,0xCD,0x61,0x10,0x7C,0x15,0x36,0x3D,0x6E,0x43,0x5C,0x96,0x81,0xE6,0x04};
    const std::array<uint8_t,32> textDigest{
        0x09,0xC8,0x40,0xF2,0x30,0x4D,0xFF,0x3A,0x73,0x95,0x3B,0xD8,0x8B,0x98,0xC7,0xC6,
        0x37,0xDB,0x73,0xFD,0x5E,0xA5,0xE2,0x25,0xA1,0xAE,0xDC,0x3F,0x4F,0xF8,0x43,0xE9};
    const auto fixture=std::filesystem::absolute("native-kernel-publication-fixture");
    require(!std::filesystem::exists(fixture),"fixture exclusively owned");
    std::filesystem::create_directory(fixture);
    struct Cleanup {std::filesystem::path root;~Cleanup(){std::filesystem::remove_all(root);}} cleanup{fixture};
    auto writeFile=[&](size_t bytes){std::ofstream out(fixture/"KERNEL.XFF",std::ios::binary|std::ios::trunc);std::vector<char> zeros(bytes);out.write(zeros.data(),zeros.size());};
    writeFile(FileBytes);
    NativeFiles files(fixture.generic_string());DataMemory memory;R5900Context context;
    auto reg=[&](unsigned index,uint32_t value){context.r[index]=_mm_set_epi32(0,0,0,value);};
    reg(4,2);reg(17,0x1B32A0);reg(28,0x141D70);reg(31,0x101D3C);
    auto word=[&](uint32_t address,uint32_t value){require(memory.write(address,&value,4),"fixture word write");};
    require(memory.write(Header,header.data(),sizeof(header)),"loaded header fixture");
    for(const auto &section:sections) require(memory.write(Table+section.index*32,section.words.data(),32),"loaded section fixture");
    word(0x131C44,Header);word(0x139E00,0x290F2C);word(0x139E04,0x290F2C);
    memory.mainRam()[Text]=0x35;memory.mainRam()[Text+TextBytes-1]=0x7B;
    unsigned hashes=0;
    Hasher hash=[&](std::span<const uint8_t> bytes){
        ++hashes;
        if(bytes.size()==FileBytes) {
            require(std::all_of(bytes.begin(),bytes.end(),[](auto b){return b==0;}),"hasher receives actual complete file fixture");
            return rawDigest;
        }
        require(bytes.size()==TextBytes && bytes.front()==0x35 && bytes.back()==0x7B,"hasher receives exact loaded static text span");
        return textDigest;
    };
    const std::vector<uint8_t> original(memory.mainRam().begin(),memory.mainRam().end());
    validateKernelPublication(memory,context,files,hash);
    require(hashes==2,"file and loaded static text separately verified");
    require(std::equal(original.begin(),original.end(),memory.mainRam().begin()),"successful validation never changes game data");
    auto rejected=[&](const Hasher &hasher=Hasher{}) {
        bool failed=false;std::string message;
        try{validateKernelPublication(memory,context,files,hasher?hasher:hash);}catch(const std::exception &error){failed=true;message=error.what();}
        require(failed,"invalid publication must fail closed");
        return message;
    };
    for(const auto index:{4u,17u,28u,31u}) {
        const auto previous=getRegU32(&context,index);reg(index,previous^4);rejected();reg(index,previous);
    }
    for(const auto address:{0x131C44u,0x139E00u,0x139E04u}) {
        uint32_t previous=0;require(memory.read(address,&previous,4),"fixture control read");word(address,previous^4);rejected();word(address,previous);
    }
    for(unsigned i=0;i<header.size();++i) {
        word(Header+i*4,header[i]^1);rejected();word(Header+i*4,header[i]);
    }
    for(const auto &section:sections) for(unsigned i=0;i<section.words.size();++i) {
        const auto address=Table+section.index*32+i*4;
        word(address,section.words[i]^1);rejected();word(address,section.words[i]);
    }
    word(Header+0x30,Table+4);const auto headerMessage=rejected();word(Header+0x30,Table);
    require(headerMessage.find("header offset=0x030 address=0x001aa7f0")!=std::string::npos &&
            headerMessage.find("actual=0x001aa874 expected=0x001aa870")!=std::string::npos,"header diagnostic identifies exact pointer mismatch");
    word(Table+32+8,TextBytes^4);const auto sectionMessage=rejected();word(Table+32+8,TextBytes);
    require(sectionMessage.find(".text offset=0x008 address=0x001aa898")!=std::string::npos &&
            sectionMessage.find("actual=0x000341a0 expected=0x000341a4")!=std::string::npos,"section diagnostic identifies word and values");
    word(0x139E04,0x290F28);const auto allocationMessage=rejected();word(0x139E04,0x290F2C);
    require(allocationMessage.find("allocation cursor offset=0x004 address=0x00139e04")!=std::string::npos &&
            allocationMessage.find("actual=0x00290f28 expected=0x00290f2c")!=std::string::npos,"allocation diagnostic identifies actual and expected placement");
    Hasher wrongRaw=[&](std::span<const uint8_t> bytes){require(bytes.size()==FileBytes,"bad raw digest stops before text hash");return std::array<uint8_t,32>{};};
    rejected(wrongRaw);
    Hasher wrongText=[&](std::span<const uint8_t> bytes){return bytes.size()==FileBytes?rawDigest:std::array<uint8_t,32>{};};
    rejected(wrongText);
    Hasher throwing=[](std::span<const uint8_t>)->std::array<uint8_t,32>{throw std::runtime_error("hasher failure");};
    rejected(throwing);
    require(std::equal(original.begin(),original.end(),memory.mainRam().begin()),"failed validation never changes game data");
    bool failed=false;try{validateKernelPublication(memory,context,files,{});}catch(const std::exception &){failed=true;}
    require(failed,"missing hashing implementation rejected");
    writeFile(FileBytes-1);rejected();writeFile(FileBytes+1);rejected();
    std::filesystem::remove(fixture/"KERNEL.XFF");rejected();writeFile(FileBytes);
    validateKernelPublication(memory,context,files,hash);
    std::array<uint64_t,NativeFiles::MaxLive> tokens{};
    for(auto &token:tokens){token=files.open("KERNEL.XFF");require(token!=0,"validation leaves no file token leak");}
    rejected();
    for(const auto token:tokens) require(files.close(token),"fixture caller tokens close");
    std::printf("native kernel publication: %u checks passed\n",checks);
}
