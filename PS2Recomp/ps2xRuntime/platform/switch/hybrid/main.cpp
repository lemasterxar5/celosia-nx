#include "ps2_runtime.h"
#include "raylib.h"
#include "native_faults.hpp"
#include "runtime/native_tuning.h"
#include "thread_placement.hpp"
#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <cxxabi.h>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {constexpr const char *Root="sdmc:/switch/celosia-native-boot";}
// nvdrv bookkeeping needs more than libnx's 8-MiB default for this scene's
// thousands of native resources. This is CPU/driver transfer memory, separate
// from the bounded GPU resource arena and from console clock settings.
extern "C" {u32 __nx_nv_transfermem_size=32u*1024u*1024u;}
void prepareSwitchDiagnostics();
void recordSwitchTermination();
static void recordHardwareClocks()
{
    const Result initialized=clkrstInitialize();
    auto *report=std::fopen("sdmc:/switch/celosia-native-boot/hardware-clocks.txt","w");
    std::fprintf(stderr,"[hardware] clkrst=0x%x operation-mode=%u\n",initialized,unsigned(appletGetOperationMode()));
    if(report)std::fprintf(report,"clkrst_result=0x%x\noperation_mode=%u\n",initialized,unsigned(appletGetOperationMode()));
    if(R_FAILED(initialized)){if(report)std::fclose(report);return;}
    const struct {PcvModuleId id;const char *name;} modules[]={
        {PcvModuleId_CpuBus,"CPU"},{PcvModuleId_GPU,"GPU"},{PcvModuleId_EMC,"MEM"}};
    for(const auto &module:modules){
        ClkrstSession session{};u32 hz=0;
        Result result=clkrstOpenSession(&session,module.id,3);
        if(R_SUCCEEDED(result)){
            result=clkrstGetClockRate(&session,&hz);
            clkrstCloseSession(&session);
        }
        std::fprintf(stderr,"[hardware] %s_hz=%u result=0x%x\n",module.name,hz,result);
        if(report)std::fprintf(report,"%s_hz=%u\n%s_result=0x%x\n",module.name,hz,module.name,result);
    }
    clkrstExit();
    if(report && std::fclose(report)!=0)std::fputs("[hardware] clock report write failed\n",stderr);
}
int main()
{
    mkdir("sdmc:/switch",0777);mkdir(Root,0777);
    if(!std::freopen("sdmc:/switch/celosia-native-boot/hybrid-boot.log","w",stdout))return 2;
    if(!std::freopen("sdmc:/switch/celosia-native-boot/hybrid-error.log","w",stderr))return 2;
    setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
    std::fprintf(stderr,"[hardware] nv_transfermem_bytes=%u\n",__nx_nv_transfermem_size);
    recordHardwareClocks(); // Read-only; never changes clock rates.
    celosia::native::placeSwitchThread("main",-1); // Record inherited affinity.
    prepareSwitchDiagnostics();
    std::set_terminate([]{
        recordSwitchTermination();
        try{if(auto error=std::current_exception())std::rethrow_exception(error);}
        catch(const std::exception &error){
            char message[768];const int count=std::snprintf(message,sizeof(message),"TERMINATED: %s\n",error.what());
            if(count>0)svcOutputDebugString(message,static_cast<size_t>(count)<sizeof(message)?count:sizeof(message)-1);
        }
        catch(...){
            const auto *type=abi::__cxa_current_exception_type();
            char message[768];const int count=std::snprintf(message,sizeof(message),"TERMINATED: exception type %s\n",type?type->name():"unknown");
            if(count>0)svcOutputDebugString(message,static_cast<size_t>(count)<sizeof(message)?count:sizeof(message)-1);
        }
        std::abort();
    });
    celosia::native::NativeFaults faults;
    if(!faults.initialize())return 2;
    std::puts("Celosia Switch: native ARM game code and direct deko3d; secondary compatibility runtime");
    if(R_FAILED(romfsInit())){std::fputs("Shader ROMFS initialization failed\n",stderr);return 2;}
    struct Romfs {~Romfs(){romfsExit();}} romfs;
    struct Audio {~Audio(){CloseAudioDevice();}} audio;
    try {
        if(chdir(Root)!=0)throw std::runtime_error("Cannot select application directory");
        // Optional local E2E input replay. No script means normal HID controls.
        if(auto *file=std::fopen("input-script.txt","rb")) {
            std::unique_ptr<FILE,decltype(&std::fclose)> input(file,&std::fclose);
            char script[4097]{};
            const auto count=std::fread(script,1,sizeof(script)-1,file);
            if(std::ferror(file) || std::fgetc(file)!=EOF || std::memchr(script,0,count))
                throw std::runtime_error("Invalid or oversized local input replay");
            if(setenv("PS2X_INPUT_SCRIPT",script,1)!=0)
                throw std::runtime_error("Cannot configure local input replay");
            std::puts("Local E2E input replay enabled");
        }else if(errno!=ENOENT)throw std::runtime_error("Cannot read local input replay");
        setenv("SOTC_NATIVE","1",1);
        setenv("SOTC_CAP30","0",1);
        // Use the existing real-file synchronous path until the desktop
        // detached CD/read-ahead workers have a Switch lifecycle implementation.
        setenv("PS2X_CD_ASYNC","0",1);
        setenv("PS2X_CD_READAHEAD","0",1);
        // Keep native algorithms on the game worker until their optional
        // detached desktop offload workers have explicit Switch ownership.
        setenv("SOTC_TEX64","0",1);
        setenv("SOTC_ASYNC_BUILDS","0",1);
        setenv("SOTC_HOOK_ASYNC","0",1);
        setenv("SOTC_MDS_LANE","0",1);
        setenv("SOTC_WORKER91","0",1);
        setenv("SOTC_LOG_ASYNC","0",1);
        setenv("SOTC_LOG76","0",1);
        setenv("PS2X_STATS","1",1); // development progression diagnostics
        setenv("PS2X_GS_HW","0",1);
        auto runtime=std::make_unique<PS2Runtime>();
        if(!runtime->initialize("Celosia Switch | deko3d"))throw std::runtime_error("Runtime initialization failed");
        // Development profile: keep native scene geometry/materials and vertex
        // fog, while the remaining deko post-processing passes are implemented.
        auto &graphics=native_renderer::tuning();
        graphics.zFog=false;graphics.bloom=false;graphics.glow=false;
        graphics.depthOfField=false;graphics.screenBlur=false;graphics.feedback=false;
        graphics.reflections=false;graphics.refraction=false;graphics.heatHaze=false;
        graphics.shadows=true;graphics.bodyShadows=true;
        std::puts("Development graphics: vertex fog and native stencil shadows; post effects/reflections pending");
        if(!runtime->loadELF("sdmc:/switch/celosia-native-boot/assets/SCUS_974.72"))throw std::runtime_error("Private game ELF load failed");
        std::puts("Private ELF loaded; starting game");
        runtime->run();
        std::puts("Game runtime returned");
        return 0;
    }catch(const std::exception &error){std::fprintf(stderr,"BLOCKED: %s\n",error.what());return 1;}
}
