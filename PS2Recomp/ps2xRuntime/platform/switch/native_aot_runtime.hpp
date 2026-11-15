#pragma once
#include "native_aot_context.hpp"
#include "native_memory.hpp"
#include <functional>
#include <atomic>
#include <stdexcept>

// The class/function names retain the static compiler ABI. This implementation
// only calls compiled host functions; it has no instruction decoder or devices.
namespace celosia::native { class BootServices; }
class PS2Runtime final {
public:
    PS2Runtime();
    ~PS2Runtime();
    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    enum class GuestBranchKind { DirectJump, DirectCall, IndirectJump, IndirectCall, Return };
    enum class SliceResult { Returned, Yielded, Stopped };
    using SyscallHandler = std::function<bool(uint8_t *, R5900Context &, uint32_t)>;
    static constexpr uint32_t ReturnSentinel = 0xFFFFFFFC;
    static constexpr uint32_t MaxCallDepth = 256;
    celosia::native::DataMemory &memory() noexcept { return data; }
    const celosia::native::DataMemory &memory() const noexcept { return data; }
    celosia::native::BootServices &services() noexcept;
    RecompiledFunction guestFunctionAt(uint32_t target) const noexcept;
    template<uint32_t Base, uint32_t Slots>
    static RecompiledFunction guestFunctionIn(uint32_t target) noexcept;
    SliceResult runSlice(R5900Context &context, uint32_t workBudget);
    bool eeCheckpointDue(uint32_t work = 32) noexcept;
    bool eeCheckpointDueAt(R5900Context *ctx, uint32_t pc, uint32_t work = 32) noexcept;
    bool callGuestFn(RecompiledFunction fn, uint8_t *ram, R5900Context *ctx, uint32_t target,
                     uint32_t returnPc, uint32_t sourcePc, GuestBranchKind kind, const char *name);
    bool callGuestKnown(RecompiledFunction fn, RecompiledFunction, uint8_t *ram, R5900Context *ctx,
                        uint32_t target, uint32_t returnPc, uint32_t sourcePc, GuestBranchKind kind, const char *name)
    { return callGuestFn(fn, ram, ctx, target, returnPc, sourcePc, kind, name); }
    void jumpGuestFn(RecompiledFunction fn, uint8_t *ram, R5900Context *ctx, uint32_t target);
    void jumpGuest(uint8_t *ram, R5900Context *ctx, uint32_t target)
    { jumpGuestFn(guestFunctionAt(target), ram, ctx, target); }
    static void tailGuestKnown(RecompiledFunction fn, RecompiledFunction, uint8_t *ram,
                               R5900Context *ctx, PS2Runtime *runtime, uint32_t target)
    { runtime->jumpGuestFn(fn, ram, ctx, target); }
    bool dispatchGuestBranch(uint8_t *ram, R5900Context *ctx, uint32_t target, uint32_t source,
                             uint32_t fallthrough, GuestBranchKind kind, const char *name);
    void setSyscallHandler(SyscallHandler handler) { syscalls = std::move(handler); }
    void handleSyscall(uint8_t *ram, R5900Context *ctx, uint32_t encodedId);
    [[noreturn]] void handleBreak(uint8_t *, R5900Context *);
    [[noreturn]] void SignalException(R5900Context *, PS2Exception);
    void clearLLBit(R5900Context *ctx) { ctx->llbit = ctx->lladdr = 0; }
    void requestStop() noexcept;
    // The operation may touch only native synchronization/local host data.
    // Game memory/context access resumes after reacquiring the execution gate.
    void nativeBlocking(const std::function<void()> &operation);
private:
    celosia::native::DataMemory data;
    std::unique_ptr<celosia::native::BootServices> boot;
    SyscallHandler syscalls;
    std::atomic<bool> stopped{false};
    // Retained storage keeps the previously compiled native AOT class ABI.
    // Actual execution state is per OS thread on each runSlice stack.
    [[maybe_unused]] uint32_t remaining = 0, depth = 0;
    [[maybe_unused]] bool yielded = false, running = false;
    void invoke(RecompiledFunction fn, uint8_t *ram, R5900Context *ctx);
};
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
template<uint32_t Base, uint32_t Slots>
PS2Runtime::RecompiledFunction PS2Runtime::guestFunctionIn(uint32_t target) noexcept
{
    const uint32_t slot = (target - Base) >> 2;
    return !(target & 3u) && target >= Base && slot < Slots ? g_ps2RecompiledFunctionTable[slot] : nullptr;
}
inline constexpr uint32_t PS2_RAM_SIZE = celosia::native::DataMemory::MainBytes;
inline constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1;
inline constexpr const uint8_t *g_ps2ReadWatchMap = nullptr;
inline constexpr void (*g_ps2ReadWatchHook)(uint32_t, uint32_t, const void *) = nullptr;
extern bool g_ps2FpuSetcsrNearest;
