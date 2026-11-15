#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include <stdexcept>
#include <cstdio>
#include <exception>

bool g_ps2FpuSetcsrNearest = false;
PS2Runtime::PS2Runtime() : boot(std::make_unique<celosia::native::BootServices>(data)) {}
PS2Runtime::~PS2Runtime()
{
    requestStop();
    if(!boot->threads().joinAll()) {
        std::fprintf(stderr,"Native runtime: workers could not join before data destruction\n");
        std::terminate();
    }
}
celosia::native::BootServices &PS2Runtime::services() noexcept { return *boot; }
void PS2Runtime::requestStop() noexcept
{
    stopped.store(true,std::memory_order_relaxed);
    boot->semaphores().shutdown();
    boot->threads().requestShutdown();
}
namespace {
struct ContextLease {
    celosia::native::BootServices &services;
    R5900Context *context;
    ContextLease(celosia::native::BootServices &s,R5900Context &ctx) : services(s),context(&ctx)
    { services.reserveContext(context); }
    ~ContextLease() { services.releaseContext(context); }
};
struct Execution {
    PS2Runtime *owner;
    ContextLease context;
    std::unique_lock<std::mutex> gate;
    uint32_t remaining,depth=0;
    bool yielded=false;
    Execution(PS2Runtime &runtime,R5900Context &ctx,uint32_t budget)
        : owner(&runtime),context(runtime.services(),ctx),gate(runtime.services().executionMutex()),remaining(budget) {}
};
thread_local Execution *active=nullptr;
[[noreturn]] void unavailable(const char *what, uint32_t value)
{
    char message[128];
    std::snprintf(message, sizeof(message), "Native runtime: %s 0x%08x is not ported", what, value);
    throw std::runtime_error(message);
}
struct DepthGuard {
    uint32_t &depth;
    explicit DepthGuard(uint32_t &d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
};
}
PS2Runtime::RecompiledFunction PS2Runtime::guestFunctionAt(uint32_t target) const noexcept
{
    const uint32_t slot = (target - g_ps2RecompiledFunctionTableBase) >> 2;
    return !(target & 3u) && target >= g_ps2RecompiledFunctionTableBase &&
        slot < g_ps2RecompiledFunctionTableSlotCount ? g_ps2RecompiledFunctionTable[slot] : nullptr;
}
bool PS2Runtime::eeCheckpointDue(uint32_t work) noexcept
{
    if(!active || active->owner!=this) return true;
    auto &execution=*active;
    if (stopped.load(std::memory_order_relaxed) || execution.yielded || work >= execution.remaining) {
        execution.yielded = true; return true;
    }
    execution.remaining -= work;
    return false;
}
bool PS2Runtime::eeCheckpointDueAt(R5900Context *ctx, uint32_t pc, uint32_t work) noexcept
{
    if (!eeCheckpointDue(work)) return false;
    ctx->pc = pc;
    return true;
}
void PS2Runtime::invoke(RecompiledFunction fn, uint8_t *ram, R5900Context *ctx)
{
    if (ram != data.mainRam().data()) throw std::invalid_argument("Native runtime: foreign data base");
    if(!active || active->owner!=this || !active->gate.owns_lock())
        throw std::logic_error("Native runtime: compiled call outside execution gate");
    boot->library().requirePublishedEntry(ctx->pc);
    if (boot->library().handle(ctx->pc,*ctx)) return;
    if (!fn) unavailable("compiled function", ctx->pc);
    if (active->depth >= MaxCallDepth) { active->yielded = true; return; }
    DepthGuard guard(active->depth);
    fn(ram, ctx, this);
}
PS2Runtime::SliceResult PS2Runtime::runSlice(R5900Context &ctx, uint32_t workBudget)
{
    if(active) throw std::logic_error("Native runtime: reentrant slice");
    Execution execution(*this,ctx,workBudget);
    struct RunGuard { ~RunGuard() { active=nullptr; } } guard;
    active=&execution;
    while (!stopped.load(std::memory_order_relaxed)) {
        if (ctx.pc == ReturnSentinel) return SliceResult::Returned;
        if (!execution.remaining || execution.yielded) return SliceResult::Yielded;
        // Charge each root call too: malformed functions cannot spin the pump.
        --execution.remaining;
        invoke(guestFunctionAt(ctx.pc), data.mainRam().data(), &ctx);
    }
    return SliceResult::Stopped;
}
bool PS2Runtime::callGuestFn(RecompiledFunction fn, uint8_t *ram, R5900Context *ctx,
    uint32_t target, uint32_t returnPc, uint32_t, GuestBranchKind, const char *)
{
    ctx->pc = target;
    if (eeCheckpointDue()) return false;
    invoke(fn, ram, ctx);
    return active && active->owner==this && !active->yielded && ctx->pc == returnPc;
}
void PS2Runtime::jumpGuestFn(RecompiledFunction fn, uint8_t *ram, R5900Context *ctx, uint32_t target)
{
    ctx->pc = target;
    if (!eeCheckpointDue()) invoke(fn, ram, ctx);
}
bool PS2Runtime::dispatchGuestBranch(uint8_t *ram, R5900Context *ctx, uint32_t target,
    uint32_t source, uint32_t fallthrough, GuestBranchKind kind, const char *name)
{
    if (kind == GuestBranchKind::Return) { ctx->pc = target; return false; }
    if (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall)
        return callGuestFn(guestFunctionAt(target), ram, ctx, target, fallthrough, source, kind, name);
    jumpGuestFn(guestFunctionAt(target), ram, ctx, target);
    return false;
}
void PS2Runtime::handleSyscall(uint8_t *ram, R5900Context *ctx, uint32_t encodedId)
{
    if (!ctx || ram != data.mainRam().data())
        throw std::invalid_argument("Native runtime: invalid system service context");
    const auto id = encodedId ? encodedId : getRegU32(ctx,3);
    if(id==0x44) {
        const auto semaphore=static_cast<int32_t>(getRegU32(ctx,4));
        int32_t result=0;
        nativeBlocking([&] { result=boot->semaphores().wait(semaphore); });
        setReturnS32(ctx,result);
        return;
    }
    if (boot->handle(id,*ctx)) return;
    if (!syscalls || !syscalls(ram, *ctx, id)) unavailable("system service", id);
}
void PS2Runtime::nativeBlocking(const std::function<void()> &operation)
{
    if(!active) { operation(); return; }
    if(active->owner!=this || !active->gate.owns_lock())
        throw std::logic_error("Native runtime: nested or foreign blocking operation");
    auto &gate=active->gate;
    gate.unlock();
    try { operation(); }
    catch(...) { gate.lock(); throw; }
    gate.lock();
}
void PS2Runtime::handleBreak(uint8_t *, R5900Context *ctx) { unavailable("break at", ctx->pc); }
void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception) { unavailable("exception at", ctx->pc); }
