#include "native_save_control.hpp"
#include "native_aot_context.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
uint32_t word(DataMemory &data,uint32_t at){uint32_t value;require(data.read(at,&value,4),"read application output");return value;}
int run()
{
    const auto fixture=std::filesystem::absolute("native-save-control-test-fixture");
    require(!std::filesystem::exists(fixture),"fixture exclusively owned");std::filesystem::create_directory(fixture);
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{fixture};
    NativeSaveStore store((fixture/"saves").generic_string());DataMemory data;NativeSaveControl control(data,store);
    R5900Context context;context.r[31]=_mm_set_epi32(0,0,0,0x1000);
    auto args=[&](std::initializer_list<uint32_t> values){unsigned reg=4;for(const auto value:values)context.r[reg++]=_mm_set_epi32(0,0,0,value);};
    auto call=[&](uint32_t at){require(control.handle(at,context) && context.pc==0x1000,"native save returns to original caller");return getRegU32(&context,2);};
    auto path=[&](const char *name){require(data.write(0x1000,name,std::strlen(name)+1),"path fixture stored");};
    require(!control.handle(0x1234,context),"unknown operation remains unhandled");
    const uint32_t canary=0xDEADBEEF;data.write(0x200,&canary,4);data.write(0x1FDAC0,&canary,4);
    args({0,0,0x200});require(call(0x1B07F0)==1 && word(data,0x200)==canary,"uninitialized storage cannot claim availability");
    require(call(0x1B06F8)==0 && word(data,0x1FDAC0)==canary,"native initialization creates storage without physical socket records");
    args({0,0,0x200});require(call(0x1B07F0)==0,"actual native free space available");
    const auto available=store.freeBytes();require(bool(available) && word(data,0x200)==std::min<uint64_t>(available.value/1024,UINT32_MAX),"free space uses verified1024-byte source units");
    data.write(0x200,&canary,4);args({1,0,0x200});require(call(0x1B07F0)==1 && word(data,0x200)==canary,"second profile is unavailable rather than duplicated");
    args({0,1,0x200});require(call(0x1B07F0)==1,"unsupported subslot does not fabricate storage");
    path("/BASCUS-97472nico");args({0,0,0x1000});require(call(0x1B09C0)==0,"real save directory created");
    require(call(0x1B09C0)==7,"duplicate directory returns actual exists code");
    require(call(0x1B0918)==0,"working directory changes to real folder");
    path("NICO00");args({0,0,0x1000});require(call(0x1B0A68)==0 && call(0x1B0A68)==7,"save creation is real and exclusive");
    constexpr char text[]="native save data";data.write(0x2000,text,sizeof(text));
    args({0,0,0x1000,0x80002000,0,sizeof(text)});require(call(0x1B0CB0)==0,"real native file write with data alias");
    std::array<uint8_t,sizeof(text)+4> fill;fill.fill(0xA5);data.write(0x3000,fill.data(),fill.size());
    args({0,0,0x1000,0x3000,0,0});require(call(0x1B0BB8)==0,"count-zero reads actual file remainder");
    require(std::memcmp(data.mainRam().data()+0x3000,text,sizeof(text))==0 && data.mainRam()[0x3000+sizeof(text)]==0xA5,"read copies only real payload and preserves trailing data");
    data.write(0x3000,fill.data(),fill.size());args({0,0,0x1000,0x3000,sizeof(text)-2,100});
    require(call(0x1B0BB8)==0 && data.mainRam()[0x3002]==0xA5,"positive read clamps to EOF");
    args({0,0,0x1000,0,sizeof(text),0});require(call(0x1B0BB8)==0,"zero remainder does not dereference destination");
    args({0,0,0x1000,0x3000,sizeof(text)+1,1});require(call(0x1B0BB8)==8,"read offset beyondEOF fails");
    args({0,0,0x1000,0x3000,UINT32_MAX,1});require(call(0x1B0BB8)==8,"negative source offset rejected");
    args({0,0,0x1000,0x1FFFFFF,0,2});require(call(0x1B0CB0)==8,"entire input checked before filesystem effects");
    std::array<uint8_t,sizeof(text)> persisted{};const auto read=store.readAt("BASCUS-97472nico/NICO00",0,persisted);
    require(bool(read) && std::memcmp(persisted.data(),text,sizeof(text))==0,"invalid write leaves existing save intact");
    for(const auto unsafe:{"../escape","C:/escape","bad\\name"}) {path(unsafe);args({0,0,0x1000});require(call(0x1B0A68)==8,"unsafe path cannot escape native save root");}
    path("OTHER");args({0,0,0x1000});require(call(0x1B0A68)==0,"second actual file created");
    path("SUB");args({0,0,0x1000});require(call(0x1B09C0)==0,"native child directory created");
    path("*");args({0,0,0x1000,0,0,0,0x4000});require(call(0x1B0E38)==0 && word(data,0x4000)==3,"unlimited count-only enumerates actual entries");
    args({0,0,0x1000,1,1,0x5000,0x4000});require(call(0x1B0E38)==0 && word(data,0x4000)==1,"pagination skips matching entries and applies limit");
    require(std::strcmp(reinterpret_cast<const char *>(data.mainRam().data()+0x5018),"OTHER")==0,"directory record uses verified32-byte name field");
    require(std::any_of(data.mainRam().data()+0x5008,data.mainRam().data()+0x5010,[](uint8_t byte){return byte!=0;}),
            "available native modification time still produces the source date");
    args({0,0,0x1000,1,1,0,0x4000});require(call(0x1B0E38)==0 && word(data,0x4000)==1,"positive count-only limit is respected");
    args({0,0,0x1000,0,0,0x12000000,0x4000});require(call(0x1B0E38)==0 && word(data,0x4000)==3,"max-zero mode does not write or dereference records");
    data.write(0x4000,&canary,4);args({0,0,0x1000,0,1,0x1FFFFFF,0x4000});require(call(0x1B0E38)==8 && word(data,0x4000)==canary,"invalid output capacity cannot publish partial directory results");
    std::filesystem::last_write_time(fixture/"saves"/"BASCUS-97472nico"/"SUB",
        std::filesystem::file_time_type::clock::from_sys(std::chrono::system_clock::time_point{}));
    const auto undated=store.stat("BASCUS-97472nico/SUB");
    require(bool(undated) && undated.value.modifiedUnixSeconds==0,"actual directory fixture exposes unavailable modification-time sentinel");
    path("S?B");args({0,0,0x1000,0,1,0x5000,0x4000});require(call(0x1B0E38)==0 && word(data,0x4000)==1,"native wildcard enumeration matches source pattern");
    const std::array<uint8_t,8> noDate{};
    require(std::memcmp(data.mainRam().data()+0x5008,noDate.data(),noDate.size())==0,
            "unavailable native modification time remains an empty source date rather than fabricated1970");
    uint16_t attributes=0;data.read(0x5014,&attributes,2);require((attributes&0x8020)==0x8020,"directory flags reflect actual native directory");
    args({0,0});require(call(0x1B0DA8)!=0 && call(0x1B0DF0)!=0,"physical formatting operations never claim success");
    require(bool(store.stat("BASCUS-97472nico/NICO00")),"format requests preserve real application files");
    path("NICO00");args({0,0,0x1000});require(call(0x1B0B10)==0 && call(0x1B0B10)==6,"native file deletion and missing-file result");
    require(call(0x1B0F20)==0,"native cleanup completes without physical device work");
    args({0,0,0x200});require(call(0x1B07F0)==1,"ended service cannot report ready");
    std::printf("native save control: %u checks passed\n",checks);return 0;
}
}
int main(){try{return run();}catch(const std::exception &error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
