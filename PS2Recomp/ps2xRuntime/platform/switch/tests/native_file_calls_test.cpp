#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_file_api.hpp"
#include "native_files.hpp"
#include "native_assets.hpp"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    using namespace celosia::native;
    const auto fixture=std::filesystem::absolute("native-file-calls-test-fixture");
    require(!std::filesystem::exists(fixture),"exclusive file call fixture");
    std::filesystem::create_directory(fixture);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{fixture};
    {std::ofstream file(fixture/"TEST.BIN",std::ios::binary);file<<"abcdefgh";}
    constexpr uint64_t HugeSize=(uint64_t(1)<<31)+17;
#ifdef _WIN32
    const auto large=CreateFileW((fixture/"HUGE.BIN").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,0,nullptr);
    require(large!=INVALID_HANDLE_VALUE,"large sparse fixture opens");
    DWORD count=0;
    const bool sparse=DeviceIoControl(large,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&count,nullptr)!=0;
    LARGE_INTEGER end{};end.QuadPart=HugeSize-1;const char marker='x';
    const bool grown=sparse && SetFilePointerEx(large,end,nullptr,FILE_BEGIN) && WriteFile(large,&marker,1,&count,nullptr);
    CloseHandle(large);require(grown && count==1,"large sparse fixture created");
#else
    {std::ofstream file(fixture/"HUGE.BIN",std::ios::binary);file.seekp(HugeSize-1);file.put('x');}
#endif
    NativeFiles files(fixture.generic_string());
    PS2Runtime runtime;R5900Context context;
    const std::array<AssetEntry,1> entries{{{"TEST.BIN",0,8}}};
    runtime.services().configureAssets(files,entries);
    require(runtime.services().assets().initialize(),"native asset cache is genuinely ready");
    auto *ram=runtime.memory().mainRam().data();
    auto reg=[&](unsigned index,uint32_t value){context.r[index]=_mm_set_epi32(0,0,0,value);};
    auto result=[&](){return static_cast<int32_t>(getRegU32(&context,2));};
    auto path=[&](const char *value){require(runtime.memory().write(0x100,value,std::strlen(value)+1),"path data setup");reg(4,0x100);};
    path("cdrom0:\\TEST.BIN;1");reg(5,1);ps2_stubs::sceOpen(ram,&context,&runtime);
    const auto fd=result();require(fd>0,"sceOpen uses actual readonly source file");
    reg(4,fd);reg(5,0x80000200);reg(6,3);ps2_stubs::sceRead(ram,&context,&runtime);
    require(result()==3 && ram[0x200]=='a' && ram[0x202]=='c',"sceRead copies actual native bytes");
    reg(4,fd);reg(5,1);reg(6,0);ps2_stubs::sceLseek(ram,&context,&runtime);require(result()==1,"sceLseek begin");
    reg(4,fd);reg(5,static_cast<uint32_t>(-1));reg(6,2);ps2_stubs::lseek(ram,&context,&runtime);require(result()==7,"lseek signed EOF offset");
    reg(4,fd);reg(5,0x300);reg(6,16);ps2_stubs::read(ram,&context,&runtime);
    require(result()==1 && ram[0x300]=='h',"read aliases native EOF behavior");
    reg(4,fd);reg(5,0);reg(6,1);ps2_syscalls::fioLseek(ram,&context,&runtime);require(result()==8,"fioLseek current");
    reg(4,fd);reg(5,0);reg(6,1);ps2_stubs::sceRead(ram,&context,&runtime);require(result()==-1,"nonempty read rejects null destination");
    reg(4,fd);reg(5,UINT32_MAX);reg(6,0);ps2_syscalls::fioRead(ram,&context,&runtime);require(result()==0,"zero read does not dereference pointer");
    reg(4,fd);reg(5,0);reg(6,0);ps2_stubs::lseek(ram,&context,&runtime);require(result()==0,"reset source cursor");
    ram[0x1FFFFFF]=0xA5;reg(4,fd);reg(5,0x1FFFFFF);reg(6,2);ps2_syscalls::fioRead(ram,&context,&runtime);
    require(result()==-1 && ram[0x1FFFFFF]==0xA5 && runtime.services().fileApi().seek(fd,0,1)==0,"invalid output fails before cursor or data mutation");
    reg(4,fd);reg(5,0x12000000);reg(6,2);ps2_stubs::read(ram,&context,&runtime);require(result()==-1,"MMIO read destination rejected");
    reg(4,fd);reg(5,0);reg(6,99);ps2_stubs::lseek(ram,&context,&runtime);require(result()==-1,"invalid seek origin");
    reg(4,fd);reg(5,UINT32_MAX);reg(6,0);ps2_stubs::lseek(ram,&context,&runtime);
    require(result()==-1 && runtime.services().fileApi().seek(fd,0,1)==0,"negative seek leaves cursor unchanged");
    require(runtime.services().fileApi().seek(fd,INT32_MAX,0)==uint64_t(INT32_MAX),"32bit boundary setup");
    reg(4,fd);reg(5,1);reg(6,1);ps2_stubs::sceLseek(ram,&context,&runtime);
    require(result()==-1 && runtime.services().fileApi().seek(fd,0,1)==uint64_t(INT32_MAX),"source32 overflow rejected before cursor mutation");
    reg(4,fd);ps2_stubs::sceClose(ram,&context,&runtime);require(result()==0,"sceClose closes native descriptor");
    reg(4,fd);ps2_stubs::close(ram,&context,&runtime);require(result()==-1,"stale close rejected");
    for(const auto flags:{0u,2u,3u,0x101u,0x201u,0x401u,UINT32_MAX}) {
        path("TEST.BIN");reg(5,flags);ps2_stubs::open(ram,&context,&runtime);require(result()==-1,"only verified PS2 readonly flag accepted");
    }
    path("TEST.BIN");reg(5,1);ps2_syscalls::fioOpen(ram,&context,&runtime);const auto resetFd=result();require(resetFd>fd,"fioOpen alias and monotonic IDs");
    ps2_stubs::sceFsReset(ram,&context,&runtime);require(result()==0 && !runtime.services().fileApi().size(resetFd),"filesystem reset really closes own descriptor");
    require(runtime.services().assets().search("TEST.BIN").has_value(),"filesystem reset preserves cached asset token");
    require(runtime.services().assets().readAtDataUnits(0,1,0x4000) && ram[0x4000]=='a' && ram[0x4007]=='h',"cached asset still reads actual bytes after reset");
    path("TEST.BIN");reg(5,1);ps2_stubs::open(ram,&context,&runtime);const auto loadFd=result();require(loadFd>resetFd,"reset does not reuse stale IDs");
    ps2_stubs::sceSifLoadFileReset(ram,&context,&runtime);require(result()==0 && !runtime.services().fileApi().size(loadFd),"loadfile reset performs real close");
    path("HUGE.BIN");reg(5,1);ps2_stubs::sceOpen(ram,&context,&runtime);const auto huge=result();require(huge>0,"large source file opened");
    reg(4,huge);reg(5,0);reg(6,2);ps2_stubs::sceLseek(ram,&context,&runtime);
    require(result()==-1 && runtime.services().fileApi().seek(huge,0,1)==0,"source32 rejects large EOF seek before mutation");
    reg(4,huge);ps2_syscalls::fioClose(ram,&context,&runtime);require(result()==0,"fioClose alias");
    path("../TEST.BIN");reg(5,1);ps2_stubs::sceOpen(ram,&context,&runtime);require(result()==-1,"native path confinement retained");
    reg(4,0);reg(5,1);ps2_stubs::sceOpen(ram,&context,&runtime);require(result()==-1,"null path rejected");
    reg(4,0x12000000);reg(5,1);ps2_stubs::sceOpen(ram,&context,&runtime);require(result()==-1,"MMIO path rejected");
    std::memset(ram+0x1000,'x',1024);reg(4,0x1000);reg(5,1);ps2_stubs::sceOpen(ram,&context,&runtime);require(result()==-1,"unterminated bounded path rejected");
    ram[0x1FFFFFF]='x';reg(4,0x1FFFFFF);reg(5,1);ps2_stubs::sceOpen(ram,&context,&runtime);require(result()==-1,"path cannot cross data boundary");
    bool rejected=false;try{ps2_stubs::sceFsReset(ram+1,&context,&runtime);}catch(const std::invalid_argument &){rejected=true;}
    require(rejected,"foreign RAM base rejected");
    rejected=false;try{ps2_stubs::sceOpen(ram,nullptr,&runtime);}catch(const std::invalid_argument &){rejected=true;}
    require(rejected,"null context rejected before register read");
    rejected=false;try{ps2_syscalls::fioRead(ram,&context,nullptr);}catch(const std::invalid_argument &){rejected=true;}
    require(rejected,"null runtime rejected");
    auto statWord=[&](unsigned offset){uint32_t value=0;require(runtime.memory().read(0x6000+offset,&value,4),"stat word read");return value;};
    path("cdrom0:\\TEST.BIN;1");reg(5,0x80006000);
    std::memset(ram+0x5fff,0xa5,42);ps2_syscalls::fioGetstat(ram,&context,&runtime);
    require(result()==0 && statWord(0)==0x15 && statWord(4)==0 && statWord(8)==8 && statWord(36)==0,
            "fioGetstat returns actual bounded readonly regular-file metadata");
    require(ram[0x5fff]==0xa5 && ram[0x6028]==0xa5,"40-byte source stat preserves surrounding canaries");
    require(ram[0x6020]>=1 && ram[0x6020]<=31 && ram[0x6021]>=1 && ram[0x6021]<=12,
            "modified timestamp has actual calendar day and month");
    path("HUGE.BIN");reg(5,0x6000);ps2_syscalls::fioGetstat(ram,&context,&runtime);
    require(result()==0 && statWord(8)==uint32_t(HugeSize) && statWord(36)==uint32_t(HugeSize>>32),
            "metadata preserves genuine file size above signed32 boundary");
    auto rejectStat=[&](const char *name,uint32_t destination){
        path(name);reg(5,destination);std::memset(ram+0x6000,0xa5,40);ram[0x1ffffff]=0x5a;
        ps2_syscalls::fioGetstat(ram,&context,&runtime);
        bool untouched=true;for(unsigned i=0;i<40;++i)untouched=untouched && ram[0x6000+i]==0xa5;
        require(result()==-1 && untouched && ram[0x1ffffff]==0x5a,"failed stat leaves all output bytes untouched");
    };
    rejectStat("MISSING.BIN",0x6000);rejectStat("../TEST.BIN",0x6000);
    rejectStat("TEST.BIN",0);rejectStat("TEST.BIN",0x12000000);
    rejectStat("TEST.BIN",0x1ffffff);rejectStat("TEST.BIN",0x70000000);
    std::filesystem::create_directory(fixture/"DIRECTORY");rejectStat("DIRECTORY",0x6000);
    reg(4,0);reg(5,0x6000);ps2_syscalls::fioGetstat(ram,&context,&runtime);require(result()==-1,"null stat path rejected");
    reg(4,0x12000000);ps2_syscalls::fioGetstat(ram,&context,&runtime);require(result()==-1,"device stat path rejected");
    // Console is a genuine native FILE sink; asset descriptors stay readonly.
    reg(4,2);reg(5,0x12000000);reg(6,1);ps2_stubs::sceWrite(ram,&context,&runtime);
    require(result()==-1,"console rejects device source before output");
    reg(4,2);reg(5,0x1FFFFFF);reg(6,2);ps2_stubs::sceWrite(ram,&context,&runtime);
    require(result()==-1,"console validates complete source before output");
    reg(4,99);reg(5,0x100);reg(6,1);ps2_stubs::write(ram,&context,&runtime);
    require(result()==-1,"readonly file descriptor cannot accept console writes");
    reg(4,2);reg(5,UINT32_MAX);reg(6,0);ps2_syscalls::fioWrite(ram,&context,&runtime);
    require(result()==0,"zero console write does not dereference source");
    require(runtime.memory().write(0x900,"native console write test\n",26),"console test bytes stored");
    reg(4,2);reg(5,0x80000900);reg(6,26);ps2_stubs::sceWrite(ram,&context,&runtime);
    require(result()==26,"console copies actual native stderr bytes and returns count");
    std::printf("native file ABI calls: %u checks passed\n",checks);
}
