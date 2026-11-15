#include "ps2_runtime_macros.h"
#include <cstdio>
#include <stdexcept>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 8;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1020;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[8]{};
static unsigned checks = 0;
static void require(bool ok, const char *why) { ++checks; if (!ok) throw std::runtime_error(why); }
static void leaf(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    setReturnU32(ctx, getRegU32(ctx, 4) + 7);
    ctx->pc = 0x1010;
}
static void caller(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ctx->r[4] = _mm_set_epi32(0,0,0,5);
    if (!runtime->callGuestFn(leaf, ram, ctx, 0x1004, 0x1010, 0x1000,
                             PS2Runtime::GuestBranchKind::DirectCall, "test")) return;
    ctx->pc = PS2Runtime::ReturnSentinel;
}
static void loop(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    while (!runtime->eeCheckpointDueAt(ctx, 0x1008, 32)) ++ctx->insn_count;
}
static void recursive(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ++ctx->insn_count;
    runtime->jumpGuestFn(recursive, ram, ctx, 0x1010);
}
static void reentrant(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{ (void)runtime->runSlice(*ctx, 100); }
static void alternate(uint8_t *, R5900Context *ctx, PS2Runtime *)
{ setReturnU32(ctx, 33); ctx->pc = 0x1010; }
static void knownCaller(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (runtime->callGuestKnown(runtime->guestFunctionAt(0x1004),leaf,ram,ctx,0x1004,0x1010,0x1018,
                               PS2Runtime::GuestBranchKind::DirectCall,"override"))
        ctx->pc = PS2Runtime::ReturnSentinel;
}
static void returning(uint8_t *, R5900Context *ctx, PS2Runtime *)
{ ctx->pc = getRegU32(ctx,31); }
static void tailCaller(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ PS2Runtime::tailGuestKnown(runtime->guestFunctionAt(0x1004),leaf,ram,ctx,runtime,0x1004); }
static void jumpCaller(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ runtime->jumpGuest(ram,ctx,0x1004); }
int main()
{
    PS2Runtime runtime;
    R5900Context context;
    g_ps2RecompiledFunctionTable[0] = caller;
    g_ps2RecompiledFunctionTable[1] = leaf;
    g_ps2RecompiledFunctionTable[2] = loop;
    context.pc = 0x1000;
    require(runtime.runSlice(context, 1024) == PS2Runtime::SliceResult::Returned, "native call returns");
    require(getRegU32(&context, 2) == 12, "native callee result");
    require(runtime.guestFunctionAt(0x1000) == caller, "registered native entry");
    require(!runtime.guestFunctionAt(0x1001) && !runtime.guestFunctionAt(0xFFFF1000), "invalid function lookup");
    context.pc = 0x1008;
    require(runtime.runSlice(context, 64) == PS2Runtime::SliceResult::Yielded && context.pc == 0x1008,
            "loop yields at native checkpoint");
    require(context.insn_count == 1, "bounded loop work");
    context.pc = 0x100C;
    bool rejected = false;
    try { (void)runtime.runSlice(context, 64); } catch (const std::runtime_error &) { rejected = true; }
    require(rejected, "missing compiled entry cannot fall back");
    rejected = false;
    try { runtime.handleSyscall(runtime.memory().mainRam().data(), &context, 42); }
    catch (const std::runtime_error &) { rejected = true; }
    require(rejected, "unported system service cannot report success");
    runtime.setSyscallHandler([](uint8_t *, R5900Context &ctx, uint32_t id) {
        if (id != 42) return false;
        setReturnU32(&ctx, 99); return true;
    });
    runtime.handleSyscall(runtime.memory().mainRam().data(), &context, 42);
    require(getRegU32(&context,2) == 99, "explicit native service provider");
    context.r[3] = _mm_set_epi32(0,0,0,42);
    runtime.handleSyscall(runtime.memory().mainRam().data(), &context, 0);
    require(getRegU32(&context,2) == 99, "syscall zero resolves service from compiler ABI v1");
    context.llbit = 1; context.lladdr = 123;
    runtime.clearLLBit(&context);
    require(!context.llbit && !context.lladdr, "reservation data cleared");
    auto *ram = runtime.memory().mainRam().data();
    ps2_guest_mem::write32(ram, &context, &runtime, 0xA0000100, 0xABCDEF01);
    require(ps2_guest_mem::read32(ram, &context, &runtime, 0x100) == 0xABCDEF01, "native slow data alias");
    rejected = false;
    try { (void)ps2_guest_mem::read32(ram, &context, &runtime, 0x12000000); }
    catch (const std::out_of_range &) { rejected = true; }
    require(rejected, "hardware register read cannot enter an emulated device");
    rejected = false;
    try { (void)ps2_guest_mem::load<uint32_t>(ram, PS2_RAM_SIZE - 1); }
    catch (const std::out_of_range &) { rejected = true; }
    require(rejected, "fast AOT read cannot cross data boundary");
    rejected = false;
    try { ps2_guest_mem::store<uint64_t>(ram, PS2_RAM_SIZE - 4, 1); }
    catch (const std::out_of_range &) { rejected = true; }
    require(rejected, "fast AOT store cannot cross data boundary");
    for (uint32_t address : {0x12000000u,PS2_RAM_SIZE-1u}) {
        rejected = false;
        try { (void)Ps2FastRead32(ram, address); } catch (const std::out_of_range &) { rejected = true; }
        require(rejected, "legacy-named fast read must enforce native data bounds");
        rejected = false;
        try { Ps2FastWrite32(ram, address, 123); } catch (const std::out_of_range &) { rejected = true; }
        require(rejected, "legacy-named fast write must enforce native data bounds");
    }
    ps2_guest_mem::write8(ram, &context, &runtime, 0x70000000, 0x7B);
    require(ps2_guest_mem::read8(ram, &context, &runtime, 0xF0000000) == 0x7B, "byte slow path");
    ps2_guest_mem::write16(ram, &context, &runtime, 0x11004000, 0xABCD);
    require(ps2_guest_mem::read16(ram, &context, &runtime, 0x11004000) == 0xABCD, "halfword slow path");
    ps2_guest_mem::write64(ram, &context, &runtime, 0x80000200, 0x123456789ABCDEF0ull);
    require(ps2_guest_mem::read64(ram, &context, &runtime, 0x200) == 0x123456789ABCDEF0ull, "doubleword slow path");
    const auto wide = _mm_set_epi32(1,2,3,4);
    ps2_guest_mem::write128(ram, &context, &runtime, 0x1100C000, wide);
    const auto restored = ps2_guest_mem::read128(ram, &context, &runtime, 0x1100C000);
    require(std::memcmp(&wide, &restored, 16) == 0, "vector slow data copy");
    rejected = false;
    try { ps2_guest_mem::write32(ram, &context, &runtime, 0x12000000, 1); }
    catch (const std::out_of_range &) { rejected = true; }
    require(rejected, "hardware writes fail explicitly");
    require(runtime.memory().getVU0Data()[0] == 0xCD, "compiled matrix stack shares bounded data");
    context.pc = 0x1000;
    require(runtime.runSlice(context, 0) == PS2Runtime::SliceResult::Yielded, "empty work slice");
    g_ps2RecompiledFunctionTable[4] = recursive;
    context.pc = 0x1010; context.insn_count = 0;
    require(runtime.runSlice(context, UINT32_MAX) == PS2Runtime::SliceResult::Yielded &&
            context.insn_count == PS2Runtime::MaxCallDepth && context.pc == 0x1010,
            "deep native calls yield without exhausting host stack");
    context.pc = 0x1000;
    require(runtime.runSlice(context, 1) == PS2Runtime::SliceResult::Yielded && context.pc == 0x1004,
            "callee entry retained when work budget closes");
    require(runtime.guestFunctionIn<0x1000,8>(0x1004) == leaf &&
            !runtime.guestFunctionIn<0x1000,8>(0xFFF), "constant native table bounds");
    require(!runtime.dispatchGuestBranch(ram, &context, 0x1020, 0, 0,
            PS2Runtime::GuestBranchKind::Return, "return") && context.pc == 0x1020, "return is not a new invocation");
    require(runtime.dispatchGuestBranch(ram, &context, 0x1004, 0x1000, 0x1010,
            PS2Runtime::GuestBranchKind::DirectCall, "call") == false,
            "closed slice cannot execute another call");
    g_ps2RecompiledFunctionTable[5] = reentrant;
    context.pc = 0x1014; rejected = false;
    try { (void)runtime.runSlice(context, 100); } catch (const std::logic_error &) { rejected = true; }
    require(rejected, "reentrant native pump rejected");
    context.pc = 0x1000;
    require(runtime.runSlice(context, 1024) == PS2Runtime::SliceResult::Returned, "exception restores pump ownership");
    g_ps2RecompiledFunctionTable[1] = alternate;
    g_ps2RecompiledFunctionTable[6] = knownCaller;
    context.pc = 0x1018;
    require(runtime.runSlice(context, 1024) == PS2Runtime::SliceResult::Returned && getRegU32(&context,2) == 33,
            "native override replaces statically known callee");
    g_ps2RecompiledFunctionTable[1] = returning;
    g_ps2RecompiledFunctionTable[7] = tailCaller;
    context.r[31] = _mm_set_epi32(0,0,0,PS2Runtime::ReturnSentinel);
    context.pc = 0x101C;
    require(runtime.runSlice(context, 1024) == PS2Runtime::SliceResult::Returned, "native tail call preserves return");
    g_ps2RecompiledFunctionTable[7] = jumpCaller;
    context.pc = 0x101C;
    require(runtime.runSlice(context, 1024) == PS2Runtime::SliceResult::Returned, "native jump entry");
    const PS2Runtime &constantRuntime = runtime;
    require(constantRuntime.memory().contains(0x100,4), "const native runtime data view");
    runtime.requestStop(); context.pc = 0x1008;
    require(runtime.runSlice(context, 64) == PS2Runtime::SliceResult::Stopped, "stop request");
    std::printf("native AOT calls: %u checks passed\n", checks);
}
