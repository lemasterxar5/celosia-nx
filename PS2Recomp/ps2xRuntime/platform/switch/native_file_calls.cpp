#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_file_api.hpp"
#include <optional>
#include <string>
#include <array>
#include <ctime>
#include <limits>
#include <cstdio>

namespace {
// Verified source ABI: runtime/ps2_memory.h PS2_FIO_O_RDONLY=0x0001.
// Both original open/sceOpen providers call fioOpen with these PS2 flags.
// Do not include the old memory/device header or use the host's POSIX O_RDONLY.
constexpr uint32_t SourceReadOnly=0x0001;
constexpr size_t PathLimit=1024;
PS2Runtime &checked(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    if(!runtime || !context || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native file call: invalid data context");
    return *runtime;
}
std::optional<std::string> pathAt(const celosia::native::DataMemory &memory,uint32_t address)
{
    if(!address) return {};
    std::string path;
    for(size_t i=0;i<PathLimit;++i) {
        const uint64_t at=uint64_t(address)+i;
        char byte=0;
        if(at>UINT32_MAX || !memory.read(static_cast<uint32_t>(at),&byte,1)) return {};
        if(!byte) return path;
        path.push_back(byte);
    }
    return {};
}
void openFile(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=checked(ram,context,runtime);
    if(getRegU32(context,5)!=SourceReadOnly) {setReturnS32(context,-1);return;}
    const auto path=pathAt(native.memory(),getRegU32(context,4));
    setReturnS32(context,path?native.services().fileApi().open(*path):-1);
}
void closeFile(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=checked(ram,context,runtime);
    setReturnS32(context,native.services().fileApi().close(static_cast<int32_t>(getRegU32(context,4))));
}
void readFile(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=checked(ram,context,runtime);
    const auto destination=getRegU32(context,5),bytes=getRegU32(context,6);
    if(!destination && bytes) {setReturnS32(context,-1);return;}
    setReturnS32(context,native.services().fileApi().read(
        static_cast<int32_t>(getRegU32(context,4)),destination,bytes));
}
void seekFile(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=checked(ram,context,runtime);
    auto &api=native.services().fileApi();
    const auto descriptor=static_cast<int32_t>(getRegU32(context,4));
    const auto offset=static_cast<int32_t>(getRegU32(context,5));
    const auto origin=getRegU32(context,6);
    // Compiled game calls are serialized by the runtime execution owner. Read
    // the base without moving the cursor, then commit an ABSOLUTE checked32-bit
    // position; an EOF stat change cannot bypass the bound during a secondseek.
    const auto current=api.seek(descriptor,0,1);
    if(!current || origin>2) {setReturnS32(context,-1);return;}
    std::optional<uint64_t> base=origin==0?0:*current;
    if(origin==2) base=api.size(descriptor);
    if(!base) {setReturnS32(context,-1);return;}
    const auto position=static_cast<__int128>(*base)+offset;
    if(position<0 || position>INT32_MAX) {setReturnS32(context,-1);return;}
    const auto moved=api.seek(descriptor,static_cast<int64_t>(position),0);
    setReturnS32(context,moved?static_cast<int32_t>(*moved):-1);
}
bool mainOutput(uint32_t address,uint32_t bytes)
{
    if(!address) return false;
    for(const auto base:{0u,0x20000000u,0x80000000u,0xA0000000u}) {
        if(address<base) continue;
        const auto offset=address-base;
        if(offset<=celosia::native::DataMemory::MainBytes &&
           bytes<=celosia::native::DataMemory::MainBytes-offset) return true;
    }
    return false;
}
void writeConsole(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=checked(ram,context,runtime);
    const auto descriptor=getRegU32(context,4),source=getRegU32(context,5),bytes=getRegU32(context,6);
    if((descriptor!=1 && descriptor!=2) || bytes>INT32_MAX || (bytes && !mainOutput(source,bytes))) {
        setReturnS32(context,-1); return;
    }
    FILE *sink=descriptor==1 ? stdout : stderr;
    std::array<uint8_t,16384> block{};
    uint32_t done=0;
    while(done<bytes) {
        const auto count=std::min<uint32_t>(block.size(),bytes-done);
        if(!native.memory().read(source+done,block.data(),count))
            throw std::logic_error("Native console: validated source read failed");
        const auto written=std::fwrite(block.data(),1,count,sink);
        done+=static_cast<uint32_t>(written);
        if(written!=count || std::ferror(sink)) {
            setReturnS32(context,done ? static_cast<int32_t>(done) : -1); return;
        }
    }
    setReturnS32(context,static_cast<int32_t>(done));
}
void encodeTime(const std::optional<int64_t> &seconds,uint8_t *output)
{
    if(!seconds || *seconds<std::numeric_limits<std::time_t>::min() ||
       *seconds>std::numeric_limits<std::time_t>::max()) return;
    const auto value=static_cast<std::time_t>(*seconds);
    std::tm calendar{};
#ifdef _WIN32
    if(localtime_s(&calendar,&value)!=0) return;
#else
    if(!localtime_r(&value,&calendar)) return;
#endif
    const auto year=int64_t(calendar.tm_year)+1900;
    if(year<0 || year>UINT16_MAX) return;
    output[1]=static_cast<uint8_t>(calendar.tm_sec);
    output[2]=static_cast<uint8_t>(calendar.tm_min);
    output[3]=static_cast<uint8_t>(calendar.tm_hour);
    output[4]=static_cast<uint8_t>(calendar.tm_mday);
    output[5]=static_cast<uint8_t>(calendar.tm_mon+1);
    output[6]=static_cast<uint8_t>(year);
    output[7]=static_cast<uint8_t>(year>>8);
}
void statFile(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=checked(ram,context,runtime);
    const auto destination=getRegU32(context,5);
    // Check the entire source record BEFORE opening a file. This bridge owns
    // ordinary main data only; matrix/scratch/device destinations are invalid.
    if(!mainOutput(destination,40)) {setReturnS32(context,-1);return;}
    const auto path=pathAt(native.memory(),getRegU32(context,4));
    if(!path) {setReturnS32(context,-1);return;}
    const auto metadata=native.services().fileApi().stat(*path);
    if(!metadata) {setReturnS32(context,-1);return;}
    std::array<uint8_t,40> record{};
    const auto word=[&](unsigned offset,uint32_t value) {
        for(unsigned byte=0;byte<4;++byte) record[offset+byte]=static_cast<uint8_t>(value>>(byte*8));
    };
    // Verified io_stat_t: regular0x10 | read0x4 | execute0x1. The native asset
    // interface is read-only regardless of host filesystem write permissions.
    word(0,0x15);word(8,static_cast<uint32_t>(metadata->size));
    word(36,static_cast<uint32_t>(metadata->size>>32));
    encodeTime(metadata->created,record.data()+12);
    encodeTime(metadata->accessed,record.data()+20);
    encodeTime(metadata->modified,record.data()+28);
    setReturnS32(context,native.memory().write(destination,record.data(),record.size())?0:-1);
}
void resetFiles(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=checked(ram,context,runtime);
    setReturnS32(context,native.services().fileApi().reset()?0:-1);
}
}
namespace ps2_stubs {
void sceWrite(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {writeConsole(ram,context,runtime);}
void write(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {writeConsole(ram,context,runtime);}
void sceOpen(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {openFile(ram,context,runtime);}
void open(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {openFile(ram,context,runtime);}
void sceClose(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {closeFile(ram,context,runtime);}
void close(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {closeFile(ram,context,runtime);}
void sceRead(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {readFile(ram,context,runtime);}
void read(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {readFile(ram,context,runtime);}
void sceLseek(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {seekFile(ram,context,runtime);}
void lseek(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {seekFile(ram,context,runtime);}
void sceFsReset(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {resetFiles(ram,context,runtime);}
void sceSifLoadFileReset(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {resetFiles(ram,context,runtime);}
}
namespace ps2_syscalls {
void fioWrite(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {writeConsole(ram,context,runtime);}
void fioGetstat(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {statFile(ram,context,runtime);}
void fioOpen(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {openFile(ram,context,runtime);}
void fioClose(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {closeFile(ram,context,runtime);}
void fioRead(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {readFile(ram,context,runtime);}
void fioLseek(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) {seekFile(ram,context,runtime);}
}
