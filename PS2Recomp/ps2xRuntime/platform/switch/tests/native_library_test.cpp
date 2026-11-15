#include "native_library.hpp"
#include "native_memory.hpp"
#include "native_semaphore.hpp"
#include "native_aot_context.hpp"
#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "ps2_syscalls.h"
#include <array>
#include <cstdio>
#include <future>
#include <stdexcept>

static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
static void oldInit(uint8_t *,R5900Context *,PS2Runtime *)
{ throw std::runtime_error("old kernel patch path"); }
static void entry(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    ctx->r[31]=_mm_set_epi32(0,0,0,PS2Runtime::ReturnSentinel);
    (void)runtime->callGuestFn(oldInit,ram,ctx,0x10FDD8,PS2Runtime::ReturnSentinel,0x1000,
                               PS2Runtime::GuestBranchKind::DirectCall,"library init");
}
int main()
{
    using namespace celosia::native;
    DataMemory data;
    SemaphoreSet semaphores;
    NativeThreads threads;
    NativeLibrary library(data,semaphores,threads);
    R5900Context context;
    context.r[31]=_mm_set_epi32(0,0,0,0x1234);
    context.r[29]=_mm_set_epi32(0,0,0,0x1FFFFF0);
    require(!library.ready() && library.currentThreadId()<0,"unregistered native thread");
    require(!library.handle(0x10FDD8,context),"library port is opt-in");
    library.enableScus();
    require(!library.handle(0xDEAD,context),"unknown calls remain unhandled");
    require(library.handle(0x10FDD8,context) && library.ready(),"native library initializes");
    require(context.pc==0x1234 && getRegU32(&context,29)==0x1FFFFF0,"native return preserves data stack");
    require(library.currentThreadId()==1,"main ID belongs to actual native owner");
    auto other=std::async(std::launch::async,[&] { return library.currentThreadId(); });
    require(other.get()<0,"foreign OS thread never aliases main ID");
    std::array<int32_t,2> ids{};
    require(data.read(0x1329A0,ids.data(),sizeof(ids)) && ids[0]>0 && ids[1]>0 && ids[0]!=ids[1],"two library semaphores published");
    for(unsigned i=0;i<2;++i) {
        const auto status=semaphores.status(ids[i]);
        require(status && status->initial==1 && status->count==1 && status->max==1 && status->option==0x138250+i*16,"library semaphore data and native state agree");
    }
    require(library.handle(0x10FDD8,context),"repeat library initialization is idempotent");
    std::array<int32_t,2> repeated{}; data.read(0x1329A0,repeated.data(),sizeof(repeated));
    require(repeated==ids,"repeat initialization does not leak semaphores");
    bool displayRejected=false;
    try { (void)library.handle(0x105CE0,context); } catch(const std::runtime_error &) { displayRejected=true; }
    require(displayRejected,"display cannot silently initialize without backend");
    unsigned displayCalls=0;
    library.setDisplayInitializer([&] { ++displayCalls; });
    require(library.handle(0x105CE0,context) && displayCalls==1,"display replacement calls configured native backend");
    context.r[4]=_mm_set_epi32(0,0,0,1);
    for(const auto address:{0x1020C0u,0x104F60u,0x114940u,0x1033B0u,0x102D70u}) {
        bool missing=false;
        try { (void)library.handle(address,context); } catch(const std::runtime_error &) { missing=true; }
        require(missing,"platform initialization cannot fake an absent native provider");
    }
    std::array<unsigned,3> callbacks{};
    library.setExceptionInitializer([&]{++callbacks[0];});
    library.setPowerOffInitializer([&]{++callbacks[1];});
    library.setTransferSynchronizer([&]{++callbacks[2];});
    unsigned platformCalls=0;
    library.setPlatformInitializer([&]{++platformCalls;});
    require(library.handle(0x1033B0,context) && platformCalls==1 && context.pc==0x1234,
            "platform bootstrap calls configured native services and returns to original caller");
    library.setPlatformInitializer([]{throw std::runtime_error("native platform unavailable");});
    bool platformFailed=false;
    try{(void)library.handle(0x1033B0,context);}catch(const std::runtime_error &){platformFailed=true;}
    require(platformFailed,"required native bootstrap error cannot report success");
    unsigned imageCalls=0;
    library.setModuleImageInitializer([&](const R5900Context &){++imageCalls;return int32_t(47);});
    require(library.handle(0x102D70,context) && imageCalls==1 && getRegU32(&context,2)==47 && context.pc==0x1234,
            "high-level image initialization returns actual native provider result");
    library.setModuleImageInitializer([](const R5900Context &)->int32_t{throw std::runtime_error("unrecognized native image");});
    bool imageRejected=false;
    try{(void)library.handle(0x102D70,context);}catch(const std::runtime_error &){imageRejected=true;}
    require(imageRejected,"unverified service image cannot claim success");
    require(library.handle(0x1020C0,context) && library.handle(0x104F60,context) && library.handle(0x114940,context) &&
            callbacks==std::array<unsigned,3>{1,1,1},"source initialization routes through configured native facilities");
    require(library.handle(0x106ED0,context) && getRegU32(&context,2)==1,"compiled GetThreadId returns bound native ID");
    require(library.handle(0x110CC8,context) && static_cast<uint64_t>(_mm_cvtsi128_si64(context.r[2]))<1474560000ull,"native clock is elapsed duration");
    require(library.handle(0x110700,context) && getRegU32(&context,2)==1,"native clock stop transition");
    require(library.handle(0x110700,context) && getRegU32(&context,2)==0,"native clock already stopped");
    require(library.handle(0x110670,context) && getRegU32(&context,2)==0,"native clock restart transition");
    require(library.handle(0x110670,context) && getRegU32(&context,2)==1,"native clock already running");
    bool blocked=false;
    try { (void)library.handle(0x10FCC8,context); } catch(const std::runtime_error &) { blocked=true; }
    require(blocked,"kernel patch entry is forbidden rather than fabricated");
    require(library.handle(0x110588,context) && getRegU32(&context,2)==0,"end real scalar timer");
    require(library.handle(0x110588,context) && getRegU32(&context,2)==0x80008001,"inactive timer error");
    context.r[4]=_mm_set_epi32(0,0,0,0);
    blocked=false;
    try { (void)library.handle(0x110430,context); } catch(const std::runtime_error &) { blocked=true; }
    require(blocked,"unsupported timer selection rejected");
    context.r[4]=_mm_set_epi32(0,0,0,2);
    require(library.handle(0x110430,context) && getRegU32(&context,2)==0,"init supported scalar timer");
    require(library.handle(0x110430,context) && getRegU32(&context,2)==0x80008002,"duplicate timer init error");
    SemaphoreSet closed;
    closed.shutdown();
    NativeLibrary failed(data,closed,threads); failed.enableScus();
    blocked=false;
    try { (void)failed.handle(0x10FDD8,context); } catch(const std::runtime_error &) { blocked=true; }
    require(blocked && !failed.ready(),"failed resource init cannot report ready");
    int32_t released=0;
    {
        NativeLibrary temporary(data,semaphores,threads); temporary.enableScus();
        (void)temporary.handle(0x10FDD8,context);
        (void)data.read(0x1329A0,&released,sizeof(released));
        require(semaphores.status(released).has_value(),"library owns native resources while alive");
    }
    require(!semaphores.status(released),"library releases owned resources on destruction");
    require(threads.shutdown(),"standalone native thread registry joins before next runtime");
    PS2Runtime runtime;
    R5900Context integrated; integrated.pc=0x1000;
    g_ps2RecompiledFunctionTable[0]=entry;
    blocked=false;
    try { (void)runtime.runSlice(integrated,128); } catch(const std::runtime_error &) { blocked=true; }
    require(blocked,"disabled port preserves compiled call behavior");
    integrated.pc=0x1000;
    runtime.services().library().enableScus();
    require(runtime.runSlice(integrated,128)==PS2Runtime::SliceResult::Returned &&
            runtime.services().library().ready(),"known compiled call reaches native replacement through runtime");
    integrated.r[4]=_mm_setzero_si128();
    runtime.memory().mainRam()[0x200]=0xA5;
    ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);
    require(getRegU32(&integrated,2)==0 && runtime.memory().mainRam()[0x200]==0xA5,"native cache clean preserves data");
    integrated.r[4]=_mm_set_epi32(0,0,0,2);
    blocked=false;
    try { ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime); }
    catch(const std::runtime_error &) { blocked=true; }
    require(blocked,"instruction cache mode cannot execute changed source instructions");
    auto &moduleLibrary=runtime.services().library();
    blocked=false;
    try { moduleLibrary.requirePublishedEntry(0x1B32A0); } catch(const std::runtime_error &) { blocked=true; }
    require(blocked,"native kernel entry requires verified data publication");
    unsigned publications=0;
    moduleLibrary.setModulePublisher([&](const R5900Context &value) {
        require(getRegU32(&value,17)==0x1B32A0,"publisher receives actual module entry context"); ++publications;
    });
    integrated.r[31]=_mm_set_epi32(0,0,0,0x101D3C);
    integrated.r[17]=_mm_set_epi32(0,0,0,0x1B32A0);
    ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);
    moduleLibrary.requirePublishedEntry(0x1B32A0);
    require(publications==1 && getRegU32(&integrated,2)==0,"module publication invokes verifier before native data synchronization");
    for(const auto init:{0x1CBC48u,0x1CB8E8u}) {
        const auto readiness=init==0x1CBC48u?0x1E1574u:0x1E156Cu;
        const uint32_t stale=1;runtime.memory().write(readiness,&stale,4);
        require(moduleLibrary.handle(init,integrated) && getRegU32(&integrated,2)==0,
                "absent optional input initializes without physical RPC transport");
        uint32_t value=1;runtime.memory().read(readiness,&value,4);
        require(value==0,"absent optional mouse and keyboard remain unavailable");
    }
    blocked=false;
    try{(void)moduleLibrary.handle(0x1B06F8,integrated);}catch(const std::runtime_error &){blocked=true;}
    require(blocked,"save bootstrap cannot fall through to physical card code without a provider");
    unsigned saveCalls=0;
    moduleLibrary.setSaveHandler([&](uint32_t address,R5900Context &value){
        ++saveCalls;setReturnU32(&value,7);return address==0x1B06F8;
    });
    require(moduleLibrary.handle(0x1B06F8,integrated) && saveCalls==1 && getRegU32(&integrated,2)==7,
            "verified save entry invokes configured provider and preserves its actual result");
    blocked=false;
    try{(void)moduleLibrary.handle(0x1B07F0,integrated);}catch(const std::logic_error &){blocked=true;}
    require(blocked,"unhandled save operation cannot silently fall through to card implementation");
    blocked=false;
    try{(void)moduleLibrary.handle(0x1C2B50,integrated);}catch(const std::runtime_error &){blocked=true;}
    require(blocked,"graphics bootstrap cannot claim an absent native backend");
    unsigned graphicsCalls=0;moduleLibrary.setGraphicsInitializer([&]{++graphicsCalls;});
    require(moduleLibrary.handle(0x1C2B50,integrated) && graphicsCalls==1 && getRegU32(&integrated,2)==0x213810,
            "graphics bootstrap initializes actual native renderer and returns original data base");
    std::array<uint32_t,2> graphicsState{};
    require(runtime.memory().read(0x2138B0,graphicsState.data(),sizeof(graphicsState)) &&
            graphicsState==std::array<uint32_t,2>{1,0},"graphics library metadata reflects initialized native backend");
    uint16_t group=1;runtime.memory().read(0x2132C8,&group,2);
    require(group==0,"native draw group begins empty");
    auto &sound=runtime.services().services();sound.initialize();
    require(sound.services().loadModule("SG2IOPM1.IRX").moduleId>0,"native sound provider loaded for library integration");
    const uint32_t emptyTick=0x1CC028;runtime.memory().write(0x273D60,&emptyTick,4);
    require(moduleLibrary.handle(0x1D23D8,integrated) && getRegU32(&integrated,2)==0,
            "verified sound entry reaches actual native SG2 controller");
    integrated.r[31]=_mm_set_epi32(0,0,0,0x101D40); blocked=false;
    try {ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);}catch(const std::runtime_error &){blocked=true;}
    require(blocked && publications==1,"instruction mode is rejected outside verified module publication callsite");
    integrated.r[31]=_mm_set_epi32(0,0,0,0x101D3C);
    moduleLibrary.setModulePublisher([](const R5900Context &){throw std::runtime_error("invalid module data");});
    blocked=false;try{ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);}catch(const std::runtime_error &){blocked=true;}
    require(blocked,"failed verifier cannot publish module");
    blocked=false;try{moduleLibrary.requirePublishedEntry(0x1B32A0);}catch(const std::runtime_error &){blocked=true;}
    require(blocked,"failed publication revokes prior module readiness");
    blocked=false;try{(void)moduleLibrary.handle(0x1D21C8,integrated);}catch(const std::runtime_error &){blocked=true;}
    require(blocked,"unpublished sound entry cannot reuse stale native controller state");
    R5900Context moduleContext;moduleContext.pc=0x1B32A0;
    blocked=false;
    try{(void)runtime.runSlice(moduleContext,8);}catch(const std::runtime_error &error){
        blocked=std::string(error.what()).find("unpublished kernel entry")!=std::string::npos;
    }
    require(blocked,"compiled dispatcher rejects unpublished module before resolving native function");
    unsigned gamePublications=0;moduleLibrary.setModulePublisher([&](const R5900Context &){++gamePublications;});
    integrated.r[31]=_mm_set_epi32(0,0,0,0x1B64D0);integrated.r[17]=_mm_set_epi32(0,0,0,0x1082ED0);
    blocked=false;try{ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);}catch(const std::runtime_error &){blocked=true;}
    require(blocked && gamePublications==0,"manager publication requires the verified kernel service");
    integrated.r[31]=_mm_set_epi32(0,0,0,0x101D3C);integrated.r[17]=_mm_set_epi32(0,0,0,0x1B32A0);
    ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);
    integrated.r[31]=_mm_set_epi32(0,0,0,0x1B64D0);integrated.r[17]=_mm_set_epi32(0,0,0,0x1082ED0);
    ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);
    moduleLibrary.requirePublishedEntry(0x107B2C8);
    blocked=false;try{moduleLibrary.requirePublishedEntry(0x1339E68);}catch(const std::runtime_error &){blocked=true;}
    require(blocked,"manager publication cannot implicitly publish gamecore");
    integrated.r[17]=_mm_set_epi32(0,0,0,0x1344788);
    ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);
    moduleLibrary.requirePublishedEntry(0x1339E68);
    require(gamePublications==3,"each pinned module is independently verified before code use");
    integrated.r[31]=_mm_set_epi32(0,0,0,0x101D3C);integrated.r[17]=_mm_set_epi32(0,0,0,0x1B32A0);
    ps2_syscalls::FlushCache(runtime.memory().mainRam().data(),&integrated,&runtime);
    for(const auto address:{0x107B2C8u,0x1339E68u}) {
        blocked=false;try{moduleLibrary.requirePublishedEntry(address);}catch(const std::runtime_error &){blocked=true;}
        require(blocked,"kernel reload revokes all dependent native module code");
    }
    const auto buffers=runtime.memory().nativeDataBuffers();
    require(buffers[0].size()==DataMemory::MainBytes && buffers[1].size()==DataMemory::ScratchBytes &&
            buffers[2].size()==DataMemory::Matrix0Bytes && buffers[3].size()==DataMemory::Matrix1Bytes,
            "native cache covers every owned data buffer");
    blocked=false;
    try { ps2_syscalls::FlushCache(nullptr,&integrated,&runtime); }
    catch(const std::invalid_argument &) { blocked=true; }
    require(blocked,"foreign cache data base rejected");
    std::printf("native library: %u checks passed\n",checks);
}
