#include "ps2_runtime.h"
#include "raylib.h"
#include "native_faults.hpp"
#include "runtime/native_tuning.h"
#include "thread_placement.hpp"
#include "live_settings.hpp"
#include <switch.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <cxxabi.h>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {constexpr const char *Root="sdmc:/switch/celosia_nx";}
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
    auto *report=std::fopen("sdmc:/switch/celosia_nx/hardware-clocks.txt","w");
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
static u32 gpuClockWanted=0;
// v173s (the user's request, 2026-11-21): 460.8 MHz (the GPU's step nearest 469 MHz without passing it) while
// the frame generation, the HD textures or the PS3 quality preset is on (live gpuBoost); otherwise gpuClockWanted.
constexpr u32 GpuBoostHz=460800000u;
// v173t: the GPU clock through apm (apmSetPerformanceConfiguration), as tw2de_nx does (private/lsfg/tw2de_nx,
// sdk/src/ui/switch_apm.cpp): Horizon reimposes the clocks of the active apm configuration, so clkrst requests
// are accepted and then undone (v152-v173s: the GPU stayed at 307.2 MHz without a clock profile), and clkrst
// sessions are few (overlays need them). The configuration keeps the memory clock the console had at start
// (its EMC flavour) and is only requested in handheld mode (docked: nothing is touched). Base: GPU 384 MHz;
// boost (live gpuBoost: LSFG / HD textures / PS3 quality preset): GPU 460.8 MHz.
struct ApmChoice {uint32_t config;int gpu,emc;};
static constexpr ApmChoice ApmTable[]={
    {0x00010000u,384,1600},{0x00020004u,384,1331},{0x00020006u,384,1065},
    {0x92220007u,460,1600},{0x92220008u,460,1331}};
static int apmEmcOf(uint32_t config)
{
    switch(config){
    case 0x00010000u:case 0x00010001u:case 0x00010002u:case 0x00020000u:case 0x00020001u:case 0x00020002u:
    case 0x92220007u:case 0x92220009u:case 0x9222000Bu:return 1600;
    case 0x00020003u:case 0x00020004u:case 0x92220008u:case 0x9222000Au:case 0x9222000Cu:return 1331;
    case 0x00020005u:case 0x00020006u:return 1065;
    default:return 0;
    }
}
static void gpuClockWatchdog(void *)
{
    Result rc=apmInitialize();
    if(R_FAILED(rc)){std::fprintf(stderr,"[apm173] apm init failed 0x%x: no GPU clock control\n",rc);return;}
    int emc=0;uint32_t startConfig=0;
    {
        ApmPerformanceMode mode=ApmPerformanceMode_Invalid;
        if(R_SUCCEEDED(apmGetPerformanceMode(&mode)) && R_SUCCEEDED(apmGetPerformanceConfiguration(mode,&startConfig)))emc=apmEmcOf(startConfig);
        std::fprintf(stderr,"[apm173] start: mode %d configuration 0x%08X (memory %d MHz)\n",int(mode),startConfig,emc);
    }
    const int baseMhz=gpuClockWanted>=384000000u?384:0; // CELOSIA_DIAG_GPU_HZ lower: the base is left as it is
    int lastMode=-2,lastWant=-1;
    for(;;){
        ApmPerformanceMode mode=ApmPerformanceMode_Invalid;
        if(R_FAILED(apmGetPerformanceMode(&mode)))mode=ApmPerformanceMode_Invalid;
        const bool boost=celosia::live::settings().gpuBoost.load(std::memory_order_relaxed);
        const int want=boost?460:baseMhz;
        if(int(mode)!=lastMode || want!=lastWant){
            if(mode!=ApmPerformanceMode_Normal){
                std::fprintf(stderr,"[apm173] mode %d (docked or unknown): nothing requested\n",int(mode));
            }else if(want==0){
                std::fprintf(stderr,"[apm173] base left as it is (diagnosis)\n");
            }else{
                bool done=false;
                // the configuration with the start's memory clock first, then the others (never more RAM than at start)
                for(int pass=0;pass<2 && !done;++pass)for(const auto &c:ApmTable){
                    if(c.gpu!=want || (pass==0)!=(c.emc==emc) || (emc && c.emc>emc))continue;
                    const Result set=apmSetPerformanceConfiguration(mode,c.config);
                    std::fprintf(stderr,"[apm173] boost %s: configuration 0x%08X (GPU %d MHz, memory %d MHz) -> 0x%x\n",boost?"on":"off",c.config,c.gpu,c.emc,set);
                    if(R_SUCCEEDED(set)){done=true;break;}
                }
                if(!done)std::fprintf(stderr,"[apm173] no %d MHz configuration accepted: left as it was\n",want);
            }
            lastMode=int(mode);lastWant=want;
        }
        svcSleepThread(250'000'000ULL);
    }
}
// v173v: libnx threads with the stack allocated by libnx (nullptr), as native_audio_output / native_threads do.
// v152-v173t used threadCreate on a static stack: 0x559 every time (the watchdog and the vsync probe never ran);
// v173u's std::thread terminated (std::thread does not work here: save_states.cpp's v98 note).
static void startDetached(const char *name,void (*entry)(void *),int priority)
{
    static Thread threads[4];static int used=0;
    if(used>=4){std::fprintf(stderr,"[thread173] %s: no slot\n",name);return;}
    Thread &t=threads[used];
    Result rc=threadCreate(&t,entry,nullptr,nullptr,0x10000,priority,-2);
    if(R_SUCCEEDED(rc))rc=threadStart(&t);
    if(R_SUCCEEDED(rc))++used;
    std::fprintf(stderr,"[thread173] %s %s 0x%x\n",name,R_SUCCEEDED(rc)?"started":"failed",rc);
}
static void startGpuClockWatchdog()
{
    startDetached("apm",gpuClockWatchdog,0x3B);
}
// v155: the display's refresh rate (60 Hz, or 90 / 120 Hz with a display mod) measured on the vsync event:
// the median of 16 intervals, for the frame generation's pacing (live refreshUs).
static void vsyncProbe(void *)
{
    ViDisplay display{};
    Event vsync{};
    Result rc=viInitialize(ViServiceType_Default);
    if(R_SUCCEEDED(rc))rc=viOpenDefaultDisplay(&display);
    if(R_SUCCEEDED(rc))rc=viGetDisplayVsyncEvent(&display,&vsync);
    std::fprintf(stderr,"[display] vsync event result=0x%x\n",rc);
    if(R_FAILED(rc))return;
    uint32_t window[16]{};
    unsigned count=0,logs=0;
    uint32_t reported=0;
    uint64_t last=0;
    for(;;){
        if(R_FAILED(eventWait(&vsync,200'000'000ULL))){last=0;continue;}
        eventClear(&vsync);
        const uint64_t now=armTicksToNs(armGetSystemTick());
        if(last){
            const uint64_t d=now-last;
            if(d>=5'000'000ULL && d<=40'000'000ULL)window[count++%16]=uint32_t(d/1000u);
            if(count>=16){
                uint32_t sorted[16];
                std::memcpy(sorted,window,sizeof(sorted));
                std::sort(sorted,sorted+16);
                const uint32_t us=sorted[8];
                celosia::live::settings().refreshUs.store(us,std::memory_order_relaxed);
                if((reported==0 || (us>reported?us-reported:reported-us)>300u) && logs++<12){
                    std::fprintf(stderr,"[display] refresh %.2f Hz\n",1e6/double(us));
                    reported=us;
                }
            }
        }
        last=now;
    }
}
static void startVsyncProbe()
{
    startDetached("vsync",vsyncProbe,0x2C); // v173u: was threadCreate on a static stack (never ran)
}
[[maybe_unused]] static void requestGpuClock(const char *value) // v173t: unused (apm instead)
{
    if(!value)return;
    constexpr u32 MaxGpuHz=384000000u;
    const unsigned long requested=std::strtoul(value,nullptr,10);
    auto *report=std::fopen("sdmc:/switch/celosia_nx/hardware-clocks-request.txt","w");
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
bool startSwitchAsyncLog();
extern "C" void switchAsyncLogFlushNow();
int main()
{
    mkdir("sdmc:/switch",0777);mkdir(Root,0777);
    if(!std::freopen("sdmc:/switch/celosia_nx/hybrid-boot.log","w",stdout))return 2;
    // v169: stderr into memory, written to hybrid-error.log by a background thread (async_log.cpp).
    const bool asyncLog=startSwitchAsyncLog();
    if(!asyncLog && !std::freopen("sdmc:/switch/celosia_nx/hybrid-error.log","w",stderr))return 2;
    setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
    std::fprintf(stderr,"[log] %s\n",asyncLog?"asynchronous (memory device, background writer)":"synchronous (SD file)");
    std::atexit([]{switchAsyncLogFlushNow();});
    std::fprintf(stderr,"[hardware] nv_transfermem_bytes=%u\n",__nx_nv_transfermem_size);
    recordHardwareClocks(); // Read-only; never changes clock rates.
    celosia::native::placeSwitchThread("main",-1); // Record inherited affinity.
    prepareSwitchDiagnostics();
    std::set_terminate([]{
        switchAsyncLogFlushNow();
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
        // v173w: PS2X_STATS (the 5 s [load]/stats lines) no longer forced on: development only
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
        // v170a: the 4-core boost chosen at start (menu.cfg boost4=1, core 3 allowed): texReset's loop on its
        // offload thread, which the boost places on core 3 (CELOSIA_DIAG_NO_BOOST_TEX64=1: not).
        {
            u64 cores=0;
            bool boostAtStart=false;
            if(FILE *cfg=std::fopen("sdmc:/switch/celosia_nx/menu.cfg","r")){
                char line[128];
                while(std::fgets(line,sizeof(line),cfg))if(!std::strncmp(line,"boost4=1",8))boostAtStart=true;
                std::fclose(cfg);
            }
            const bool core3=R_SUCCEEDED(svcGetInfo(&cores,InfoType_CoreMask,CUR_PROCESS_HANDLE,0)) && (cores&8u);
            if(boostAtStart && core3 && !diagOn("CELOSIA_DIAG_NO_BOOST_TEX64")){
                setenv("SOTC_TEX64","1",1);
                std::puts("4-core boost at start: texture loop on the offload thread (core 3)");
            }
            // v171 (boost improvement): with the fourth core, also round 61's modelDispState lane and round 91's guest
            // worker (the particle / wind module's guest code beside the game thread). v117 measured them worse with
            // three cores (all saturated); the boost gives them cores 2-3 (boostPlace). CELOSIA_DIAG_NO_BOOST_OFFLOAD171=1: not.
            if(boostAtStart && core3 && diagOn("CELOSIA_DIAG_BOOST_OFFLOAD171")){ // v171b: off by default (v171-play4 46.5 FPS vs 52.7)
                setenv("SOTC_MDS_LANE","1",1);
                setenv("SOTC_WORKER91","1",1);
                std::puts("4-core boost at start (v171): modelDispState lane and guest worker on");
            }
        }
        if(diagOn("CELOSIA_DIAG_MDS_LANE"))setenv("SOTC_MDS_LANE","1",1);
        if(diagOn("CELOSIA_DIAG_WORKER91"))setenv("SOTC_WORKER91","1",1);
        if(diagOn("CELOSIA_DIAG_CD_TRACE"))setenv("PS2X_CD_TRACE","1",1);
        {
            const char *oc=std::getenv("CELOSIA_DIAG_OFFLOAD_CORE");
            const int offloadCore=oc?std::atoi(oc):1;
            // v170a: CELOSIA_DIAG_NO_FZ170=1 leaves FPCR.FZ off (0x100); CELOSIA_DIAG_COLL_FASTCHECK=1 compares the
            // collision's fast chains with the exact ones (SOTC_COLLISION_FASTCHECK).
            if(diagOn("CELOSIA_DIAG_COLL_FASTCHECK"))setenv("SOTC_COLLISION_FASTCHECK","1",1);
            // v170a: the VU0 / FPU macros against their reference forms at start, in the game thread's float
            // environment (FZ on): proof that the Vita's clamp (ps2VuOperand) is exact here (PS2X_MACRO_SELFTEST, N M).
            if(diagOn("CELOSIA_DIAG_MACRO_SELFTEST"))setenv("PS2X_MACRO_SELFTEST","20",1);
            if(diagOn("CELOSIA_DIAG_NO_DISPATCH170"))setenv("PS2X_NO_DISPATCH170","1",1); // v170a CPU #2 off
            if(diagOn("CELOSIA_DIAG_FASTNATIVE_CHECK")){setenv("SOTC_FASTNATIVE_CHECK","1",1);setenv("SOTC_FAST_CHECK","1",1);}
            celosia_thread_flags.store((diagOn("CELOSIA_DIAG_BOOST_PIN170")?0x200:0)|(diagOn("CELOSIA_DIAG_NO_FZ170")?0x100:0)|(diagOn("CELOSIA_DIAG_NO_CORE2")?1:0)|(diagOn("CELOSIA_DIAG_THREAD_LOG")?2:0)|
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
        // v152: the GPU at 384 MHz always (the user's rule: never above 387 MHz), not left to a per-title
        // clock profile. A watchdog asks again once a second if the system brought it back down.
        // CELOSIA_DIAG_GPU_HZ may still ask for a lower step (measures only).
        {
            const char *diag=std::getenv("CELOSIA_DIAG_GPU_HZ");
            // v173t: through apm (gpuClockWatchdog), no clkrst request (Horizon undid it: GPU stayed at 307.2).
            gpuClockWanted=u32(std::strtoul(diag?diag:"384000000",nullptr,10));
            startGpuClockWatchdog();
        }
        startVsyncProbe();
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
        if(!runtime->loadELF("sdmc:/switch/celosia_nx/assets/SCUS_974.72"))throw std::runtime_error("Private game ELF load failed");
        std::puts("Private ELF loaded; starting game");
        runtime->run();
        std::puts("Game runtime returned");
        return 0;
    }catch(const std::exception &error){std::fprintf(stderr,"BLOCKED: %s\n",error.what());return 1;}
}
