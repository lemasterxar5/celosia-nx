#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x100C;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=3;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[3]{};
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
static std::atomic<unsigned> inside{0},violations{0};
static void increment(uint8_t *ram,R5900Context *ctx,PS2Runtime *)
{
    if(inside.fetch_add(1)!=0) violations.fetch_add(1);
    const auto old=ram[0x100];
    std::this_thread::yield();
    ram[0x100]=old+1;
    inside.fetch_sub(1);
    ctx->pc=PS2Runtime::ReturnSentinel;
}
static int32_t semaphoreId;
static std::promise<void> entered;
static void blocking(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    ctx->r[4]=_mm_set_epi32(0,0,0,semaphoreId);
    entered.set_value();
    runtime->handleSyscall(ram,ctx,0x44);
    ctx->pc=PS2Runtime::ReturnSentinel;
}
static void throwingWait(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    bool caught=false;
    try { runtime->nativeBlocking([] { throw std::runtime_error("injected wait failure"); }); }
    catch(const std::runtime_error &) { caught=true; }
    if(!caught) throw std::runtime_error("missing wait failure");
    caught=false;
    try { runtime->nativeBlocking([&] { runtime->nativeBlocking([]{}); }); }
    catch(const std::logic_error &) { caught=true; }
    if(!caught) throw std::runtime_error("nested blocking call accepted");
    ram[0x200]=77;
    ctx->pc=PS2Runtime::ReturnSentinel;
}
int main()
{
    PS2Runtime runtime;
    g_ps2RecompiledFunctionTable[0]=increment;
    g_ps2RecompiledFunctionTable[1]=blocking;
    g_ps2RecompiledFunctionTable[2]=throwingWait;
    auto run=[&] {
        for(unsigned i=0;i<80;++i) {
            R5900Context context; context.pc=0x1000;
            if(runtime.runSlice(context,128)!=PS2Runtime::SliceResult::Returned) return false;
        }
        return true;
    };
    auto a=std::async(std::launch::async,run);
    auto b=std::async(std::launch::async,run);
    require(a.get() && b.get(),"two actual OS threads execute compiled slices");
    require(violations.load()==0 && runtime.memory().mainRam()[0x100]==160,"game data writes are serialized");
    semaphoreId=runtime.services().semaphores().create(0,1,0,0);
    R5900Context waiting; waiting.pc=0x1004;
    auto waiter=std::async(std::launch::async,[&] { return runtime.runSlice(waiting,128); });
    entered.get_future().wait();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(runtime.services().semaphores().status(semaphoreId)->waiters!=1) {
        if(std::chrono::steady_clock::now()>deadline) { runtime.requestStop(); waiter.wait(); throw std::runtime_error("waiter never blocked"); }
        std::this_thread::yield();
    }
    bool rejected=false;
    try { (void)runtime.runSlice(waiting,128); } catch(const std::logic_error &) { rejected=true; }
    require(rejected,"same context cannot execute concurrently while OS thread waits");
    auto signaler=std::async(std::launch::async,[&] {
        R5900Context context; context.pc=0x1000;
        const auto result=runtime.runSlice(context,128);
        runtime.services().semaphores().signal(semaphoreId);
        return result;
    });
    if(signaler.wait_for(std::chrono::seconds(2))!=std::future_status::ready) {
        runtime.requestStop(); waiter.wait(); signaler.wait(); throw std::runtime_error("wait held execution gate");
    }
    require(signaler.get()==PS2Runtime::SliceResult::Returned,"another native thread can run during native wait");
    require(waiter.get()==PS2Runtime::SliceResult::Returned && getRegU32(&waiting,2)==uint32_t(semaphoreId),"wait result resumes correct native context");
    R5900Context failure; failure.pc=0x1008;
    require(runtime.runSlice(failure,128)==PS2Runtime::SliceResult::Returned && runtime.memory().mainRam()[0x200]==77,
            "throwing native wait reacquires gate and restores TLS before data execution");
    entered=std::promise<void>();
    semaphoreId=runtime.services().semaphores().create(0,1,0,0);
    waiting.pc=0x1004;
    auto stopping=std::async(std::launch::async,[&] { return runtime.runSlice(waiting,128); });
    entered.get_future().wait();
    runtime.requestStop();
    require(stopping.wait_for(std::chrono::seconds(2))==std::future_status::ready &&
            stopping.get()==PS2Runtime::SliceResult::Stopped,"stop wakes native waiter before runtime/data destruction");
    std::printf("native execution: %u checks passed\n",checks);
}
