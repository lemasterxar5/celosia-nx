// Private bootstrap/heap/math integration diagnostic, not a playable game.
// Supply the locally generated entry/math sources; never embed game assets.
#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_audio_output.hpp"
#include <ps2_recompiled_functions.h>
#include <switch.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <sys/stat.h>

void iosAbsVector_0x1be898(uint8_t *, R5900Context *, PS2Runtime *);
extern const uint32_t g_ps2RecompiledFunctionTableBase = PS2X_RECOMP_TABLE_BASE;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = PS2X_RECOMP_TABLE_SLOTS;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = PS2X_RECOMP_TABLE_BASE+PS2X_RECOMP_TABLE_SLOTS*4;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[PS2X_RECOMP_TABLE_SLOTS]{};
namespace {
struct InitBoundary final : std::exception {};
void install(uint32_t address,PS2Runtime::RecompiledFunction fn)
{ g_ps2RecompiledFunctionTable[(address-PS2X_RECOMP_TABLE_BASE)/4]=fn; }
struct NativeWait {
    celosia::native::SemaphoreSet &set;
    int32_t id;
    std::atomic<int32_t> result{INT32_MIN};
};
void waitNative(void *argument)
{
    auto &task=*static_cast<NativeWait *>(argument);
    task.result.store(task.set.wait(task.id),std::memory_order_release);
}
void verifyNativeWait(celosia::native::SemaphoreSet &set,int32_t id)
{
    if(set.poll(id)!=id) throw std::runtime_error("native semaphore initial token missing");
    NativeWait task{set,id};
    Thread thread{};
    if(R_FAILED(threadCreate(&thread,waitNative,&task,nullptr,0x10000,0x2C,-2)))
        throw std::runtime_error("libnx thread creation failed");
    bool started=false;
    try {
        if(R_FAILED(threadStart(&thread))) throw std::runtime_error("libnx thread start failed");
        started=true;
        auto deadline=armGetSystemTick()+armNsToTicks(2000000000ull);
        for(;;) {
            const auto status=set.status(id);
            if(status && status->waiters==1) break;
            if(armGetSystemTick()>=deadline) throw std::runtime_error("native thread did not wait");
            svcSleepThread(1000000);
        }
        if(set.signal(id)!=id) throw std::runtime_error("native signal failed");
        deadline=armGetSystemTick()+armNsToTicks(2000000000ull);
        while(task.result.load(std::memory_order_acquire)==INT32_MIN) {
            if(armGetSystemTick()>=deadline) throw std::runtime_error("native waiter did not wake");
            svcSleepThread(1000000);
        }
        if(R_FAILED(threadWaitForExit(&thread))) throw std::runtime_error("native thread join failed");
        started=false;
        if(task.result.load()!=id) throw std::runtime_error("native waiter result mismatch");
    } catch(...) {
        set.shutdown();
        if(started) (void)threadWaitForExit(&thread);
        (void)threadClose(&thread);
        throw;
    }
    if(R_FAILED(threadClose(&thread))) throw std::runtime_error("native thread close failed");
}
}
// Diagnostic-only stop point, never linked into the native core libraries.
// Library initialization is still unported; this hook must never fake success.
void sub_0010FCC8_0x10fcc8(uint8_t *,R5900Context *,PS2Runtime *) { throw InitBoundary{}; }
#define UNPORTED_INIT(name) void name(uint8_t *,R5900Context *,PS2Runtime *) { throw std::runtime_error("unexpected " #name); }
UNPORTED_INIT(InitAlarm_0x1102b8)
UNPORTED_INIT(InitTimer_0x110430)
UNPORTED_INIT(StartTimerSystemTime_0x110670)
UNPORTED_INIT(InitThread_0x107768)
UNPORTED_INIT(InitExecPS2_0x10ff00)
UNPORTED_INIT(InitTLBFunctions_0x10f3c8)
UNPORTED_INIT(sceSifInitRebootNotify_0x111af0)
#undef UNPORTED_INIT
void FlushCache_0x107260(uint8_t *,R5900Context *,PS2Runtime *) { throw std::runtime_error("unexpected cache call"); }
void sub_00103B40_0x103b40(uint8_t *,R5900Context *,PS2Runtime *) { throw std::runtime_error("unexpected main call"); }
void Exit_0x1101d8(uint8_t *,R5900Context *,PS2Runtime *) { throw std::runtime_error("unexpected exit call"); }
int main()
{
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/celosia-native-core", 0777);
    FILE *log = std::fopen("sdmc:/switch/celosia-native-core/probe.log", "w");
    if (!log) return 1;
    std::fprintf(log, "Celosia native core diagnostic; NOT THE GAME; build %s %s\n", __DATE__, __TIME__);
    int result = 0;
    try {
        PS2Runtime runtime;
        R5900Context context;
        install(0x1BE898,iosAbsVector_0x1be898);
        for (uint32_t pc : {0x100010u,0x100020u,0x100070u,0x10008Cu,0x100094u,0x10009Cu,0x1000B4u})
            install(pc,sub_00100010_0x100010);
        for(uint32_t pc : {0x10FDD8u,0x10FDE8u,0x10FDF0u,0x10FDF8u,0x10FE00u,0x10FE08u,0x10FE10u,0x10FE18u,0x10FE20u})
            install(pc,ps2__InitSys_0x10fdd8);
        for(uint32_t pc : {0x10FB88u,0x10FBC4u,0x10FBD4u}) install(pc,sub_0010FB88_0x10fb88);
        install(0x106FE0,CreateSema_0x106fe0);
        install(0x10FCC8,sub_0010FCC8_0x10fcc8);
        context.pc=0x100010;
        bool reachedInit=false;
        try {
            for (unsigned slice=0;slice<128;++slice) (void)runtime.runSlice(context,4096);
        } catch(const InitBoundary &) { reachedInit=true; }
        if (!reachedInit || getRegU32(&context,29)!=0x1FFFFF0 || getRegU32(&context,28)!=0x141D70 ||
            runtime.services().heap().base()!=0x16A3A0 || runtime.services().heap().limit()!=0x1AA3A0)
            throw std::runtime_error("compiled bootstrap state mismatch");
        std::fprintf(log,"PASS: compiled entry/bootstrap; active library SP=0x01fffff0 GP=0x00141d70 heap=0x0016a3a0..0x001aa3a0\n");
        std::array<int32_t,2> ids{};
        if(!runtime.memory().read(0x1329A0,ids.data(),sizeof(ids)) || ids[0]<=0 || ids[1]<=0 || ids[0]==ids[1])
            throw std::runtime_error("compiled library semaphore handles missing");
        auto &semaphores=runtime.services().semaphores();
        for(unsigned i=0;i<2;++i) {
            const auto status=semaphores.status(ids[i]);
            if(!status || status->count!=1 || status->max!=1 || status->initial!=1 || status->option!=0x138250+i*16)
                throw std::runtime_error("compiled library semaphore state mismatch");
        }
        std::fprintf(log,"PASS: compiled library created two native semaphores\n");
        std::fflush(log);
        verifyNativeWait(semaphores,ids[0]);
        std::fprintf(log,"PASS: libnx thread blocked and woke through native semaphore\n");
        std::fprintf(log,"stopped BEFORE unported kernel patch/timer initialization; not game boot\n");
        auto &heap=runtime.services().heap();
        const auto block=heap.allocate(64);
        auto data=runtime.memory().mainRam();
        if (!block) throw std::runtime_error("native heap allocation failed");
        data[block]=0x5A;
        const auto grown=heap.reallocate(block,128);
        if (!grown || data[grown]!=0x5A || data[grown+64]!=0 || !heap.release(grown) || data[grown]!=0)
            throw std::runtime_error("native heap lifecycle mismatch");
        std::fprintf(log,"PASS: native heap allocate/reallocate/free preserved and cleared data\n");
        celosia::native::NativeAudioOutput audio;
        unsigned pulls=0;
        if(!audio.start([&](int16_t *,size_t)->size_t {
            if(++pulls>3) throw std::runtime_error("injected audio source failure");
            return 0;
        })) throw std::runtime_error("native audio error-path initialization failed");
        const auto failureDeadline=armGetSystemTick()+armNsToTicks(5000000000ull);
        while(audio.stats().running && armGetSystemTick()<failureDeadline) svcSleepThread(1000000);
        audio.stop();
        if(audio.stats().failure!=celosia::native::AudioFailure::Source)
            throw std::runtime_error("native audio source error not reported");
        std::fprintf(log,"PASS: injected audio source error reported and stopped safely\n");
        for(unsigned cycle=0;cycle<2;++cycle) {
            if(!audio.start([](int16_t *,size_t) { return size_t(0); }))
                throw std::runtime_error("native audout initialization failed");
            const auto audioDeadline=armGetSystemTick()+armNsToTicks(5000000000ull);
            while(audio.stats().completedBuffers<4 && audio.stats().running && armGetSystemTick()<audioDeadline)
                svcSleepThread(1000000);
            audio.stop();
            const auto audioStats=audio.stats();
            if(audioStats.failure!=celosia::native::AudioFailure::None || audioStats.completedBuffers<4)
                throw std::runtime_error("native audout buffer cycle failed");
            std::fprintf(log,"PASS: native audout stereo48k cycle=%u submitted=%llu completed_buffers=%llu (silence test)\n",
                         cycle,static_cast<unsigned long long>(audioStats.submittedFrames),
                         static_cast<unsigned long long>(audioStats.completedBuffers));
        }
        context=R5900Context{};
        const std::array<float,4> input{-1.25f,2.5f,-0.0f,-128.0f};
        std::array<float,4> output{};
        if (!runtime.memory().write(0x100, input.data(), sizeof(input)))
            throw std::runtime_error("diagnostic data write failed");
        context.r[4] = _mm_set_epi32(0,0,0,0x200);
        context.r[5] = _mm_set_epi32(0,0,0,0x100);
        context.r[31] = _mm_set_epi32(0,0,0,PS2Runtime::ReturnSentinel);
        context.pc = 0x1BE898;
        const auto outcome = runtime.runSlice(context, 1024);
        if (outcome != PS2Runtime::SliceResult::Returned ||
            !runtime.memory().read(0x200, output.data(), sizeof(output)))
            throw std::runtime_error("compiled math function did not return");
        const std::array<float,4> expected{1.25f,2.5f,0.0f,128.0f};
        if (output != expected || std::signbit(output[2]))
            throw std::runtime_error("compiled math result mismatch");
        if (runtime.memory().contains(0x12000000,4))
            throw std::runtime_error("device address unexpectedly mapped");
        std::fprintf(log, "compiled_entry=0x001be898; result=%.2f,%.2f,%.2f,%.2f\n",
                     output[0],output[1],output[2],output[3]);
        std::fprintf(log, "PASS: native AOT returned; device address rejected; no game/stock performance claim\n");
    } catch (const std::exception &error) {
        std::fprintf(log, "FAIL: %s\n", error.what()); result = 2;
    }
    std::fclose(log);
    return result;
}
