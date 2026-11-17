#include "ps2_runtime.h"
#include "raylib.h"
#include "native_faults.hpp"
#include "runtime/native_tuning.h"
#include "thread_placement.hpp"
#include <switch.h>
#include <atomic>
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
extern "C" void celosia_saltynx_start(void); // saltynx_fps.cpp
extern std::atomic<int> celosia_thread_flags; // native_renderer.cpp: helper-thread placement
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
// Optional GPU clock request (diagnosis key CELOSIA_DIAG_GPU_HZ). The project's limit is 386 MHz:
// anything above the 384 MHz step is refused, never rounded up. CPU and memory clocks are not touched.
// The applied and read-back rates go to hardware-clocks-request.txt.
static void requestGpuClock(const char *value)
{
    if(!value)return;
    constexpr u32 MaxGpuHz=384000000u;
    const unsigned long requested=std::strtoul(value,nullptr,10);
    auto *report=std::fopen("sdmc:/switch/celosia-native-boot/hardware-clocks-request.txt","w");
    if(requested==0 || requested>MaxGpuHz){
        if(report){std::fprintf(report,"gpu_requested_hz=%lu\ngpu_request=refused_over_limit\n",requested);std::fclose(report);}
        return;
    }
    Result result=clkrstInitialize();
    u32 before=0,after=0;
    if(R_SUCCEEDED(result)){
        ClkrstSession session{};
        result=clkrstOpenSession(&session,PcvModuleId_GPU,3);
        if(R_SUCCEEDED(result)){
            clkrstGetClockRate(&session,&before);
            result=clkrstSetClockRate(&session,u32(requested));
            clkrstGetClockRate(&session,&after);
            clkrstCloseSession(&session);
        }
        clkrstExit();
    }
    std::fprintf(stderr,"[hardware] gpu request %lu: before=%u after=%u result=0x%x\n",requested,before,after,result);
    if(report){
        std::fprintf(report,"gpu_requested_hz=%lu\ngpu_before_hz=%u\ngpu_after_hz=%u\ngpu_request_result=0x%x\n",requested,before,after,result);
        std::fclose(report);
    }
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
        // Optional development diagnosis: diag-env.txt lines KEY=VALUE, only CELOSIA_DIAG_* keys.
        if(auto *file=std::fopen("diag-env.txt","rb")) {
            std::unique_ptr<FILE,decltype(&std::fclose)> diag(file,&std::fclose);
            char line[160];
            while(std::fgets(line,sizeof(line),file)) {
                line[std::strcspn(line,"\r\n")]=0;
                char *eq=std::strchr(line,'=');
                if(!eq || std::strncmp(line,"CELOSIA_DIAG_",13)!=0)continue;
                *eq=0;setenv(line,eq+1,1);
                std::printf("Diagnosis setting %s=%s\n",line,eq+1);
            }
        }
        // CELOSIA_DIAG_PROF129=1: the game thread's sampling profiler (game/overrides/sotc_prof129.cpp)
        // writes [prof129] windows to hybrid-error.log; CELOSIA_DIAG_PROF129_SECS sets the window.
        if(const char *prof=std::getenv("CELOSIA_DIAG_PROF129"); prof && std::atoi(prof)!=0) {
            setenv("SOTC_PROF129","1",1);
            const char *secs=std::getenv("CELOSIA_DIAG_PROF129_SECS");
            setenv("SOTC_PROF129_SECS",secs?secs:"15",1);
        }
        // CELOSIA_DIAG_FRAME_LOG=1: the native frame log (60-frame [frame] summaries plus slow frames over
        // CELOSIA_DIAG_FRAME_SLOW_MS, default 60) in hybrid-error.log: hooks/render/present/wait/sync split.
        if(const char *log=std::getenv("CELOSIA_DIAG_FRAME_LOG"); log && std::atoi(log)!=0) {
            setenv("SOTC_FRAME_LOG","1",1);
            const char *slow=std::getenv("CELOSIA_DIAG_FRAME_SLOW_MS");
            setenv("SOTC_FRAME_SLOW_MS",slow?slow:"60",1);
        }
        // Hardware A/B switches for the helper threads (default off = v60 behaviour). Helper threads go to
        // core 2 (native_renderer::threads::place). CELOSIA_DIAG_CD_ASYNC=1: disc reads on the reader
        // thread (sceCdSync waits, guest copy on the EE executor); CELOSIA_DIAG_CD_READAHEAD=1 adds the
        // stream read-ahead; CELOSIA_DIAG_HOOK_ASYNC=1: model hooks (packets, skinning) on the hook worker.
        auto diagOn=[](const char *key){const char *v=std::getenv(key);return v && std::atoi(v)!=0;};
        // v85: CD_ASYNC and HOOK_ASYNC are on unless set to 0 (every measured build since v61 ran with
        // both; without them the model hooks run on the game thread and it saturates in battles).
        auto diagDefaultOn=[](const char *key){const char *v=std::getenv(key);return !v || std::atoi(v)!=0;};
        if(diagDefaultOn("CELOSIA_DIAG_CD_ASYNC"))setenv("PS2X_CD_ASYNC","1",1);
        if(diagOn("CELOSIA_DIAG_CD_READAHEAD"))setenv("PS2X_CD_READAHEAD","1",1);
        if(diagDefaultOn("CELOSIA_DIAG_HOOK_ASYNC"))setenv("SOTC_HOOK_ASYNC","1",1);
        // v117: the offload workers start through sotc_spawn (a Switch-safe detached thread); off unless set:
        // CELOSIA_DIAG_TEX64=1 texReset's loop, CELOSIA_DIAG_MDS_LANE=1 modelDispState's lane,
        // CELOSIA_DIAG_WORKER91=1 the particle / wind module's guest code on the guest worker.
        if(diagOn("CELOSIA_DIAG_TEX64"))setenv("SOTC_TEX64","1",1);
        if(diagOn("CELOSIA_DIAG_MDS_LANE"))setenv("SOTC_MDS_LANE","1",1);
        if(diagOn("CELOSIA_DIAG_WORKER91"))setenv("SOTC_WORKER91","1",1);
        if(diagOn("CELOSIA_DIAG_CD_TRACE"))setenv("PS2X_CD_TRACE","1",1);
        {
            const char *oc=std::getenv("CELOSIA_DIAG_OFFLOAD_CORE");
            const int offloadCore=oc?std::atoi(oc):1;
            celosia_thread_flags.store((diagOn("CELOSIA_DIAG_NO_CORE2")?1:0)|(diagOn("CELOSIA_DIAG_THREAD_LOG")?2:0)|
                                       (offloadCore>=0 && offloadCore<=2?0x40|(offloadCore<<4):0));
        }
        // The main thread runs the host loop (vblank pacing, frame setup). With its inherited mask (all
        // application cores) the kernel may put it on a different core from run to run; suspected cause of
        // the same build running the altar at 35-38 instead of 44-45 FPS (core 2 load 62 % vs 96 %).
        // It joins the helper threads on core 2. CELOSIA_DIAG_MAIN_CORE=-1 keeps the inherited mask.
        {
            int mainCore=2;
            if(const char *value=std::getenv("CELOSIA_DIAG_MAIN_CORE"))mainCore=std::atoi(value);
            celosia::native::placeSwitchThread("main-placed",mainCore);
        }
        requestGpuClock(std::getenv("CELOSIA_DIAG_GPU_HZ"));
        celosia_saltynx_start();
        auto runtime=std::make_unique<PS2Runtime>();
        if(!runtime->initialize("Celosia Switch | deko3d"))throw std::runtime_error("Runtime initialization failed");
        // Development profile: keep native scene geometry/materials and vertex
        // fog, while the remaining deko post-processing passes are implemented.
        auto &graphics=native_renderer::tuning();
        graphics.zFog=std::getenv("CELOSIA_DIAG_NO_ZFOG")==nullptr;graphics.bloom=std::getenv("CELOSIA_DIAG_NO_BLOOM")==nullptr;graphics.glow=false;
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
