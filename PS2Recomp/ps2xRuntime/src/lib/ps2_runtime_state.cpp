// Save states (v94): the whole emulated machine in one byte stream. Each part writes its own
// section (state_stream.h); this file orders them and adds the header. Host-side caches (the
// native renderer's, the CD read-ahead) are not saved: they rebuild from the restored memory.
#include "ps2_runtime.h"
#include "runtime/state_stream.h"
#include "runtime/ee_scheduler.h"
#include "ps2x/iop/iop_subsystem.h"
#include "Kernel/Stubs/CD.h"

#include <algorithm>
#include <type_traits>

namespace
{
    constexpr uint32_t kMachineStateVersion = 1u;

    // Changes when a saved structure's layout changes, so an old state is refused instead of
    // read into the wrong fields.
    uint64_t layoutFingerprint()
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(sizeof(R5900Context));
        mix(PS2_RAM_SIZE);
        mix(PS2_GS_VRAM_SIZE);
        mix(sizeof(void *));
        return h;
    }
}

bool PS2Runtime::machineSafeForState(std::string &why)
{
    if (!m_eeScheduler)
    {
        why = "no scheduler";
        return false;
    }
    if (!m_eeScheduler->safeForState(why))
        return false;
    m_memory.drainForState();
    if (!m_memory.quietForState(why))
        return false;
    if (!ps2_stubs::cdSafeForState(why))
        return false;
    if (m_iopSubsystem && !m_iopSubsystem->safeForState(why))
        return false;
    return true;
}

bool PS2Runtime::saveMachineState(std::vector<uint8_t> &bytes, std::string &why)
{
    static_assert(std::is_trivially_copyable_v<R5900Context>);
    if (!machineSafeForState(why))
        return false;
    ps2state::StateWriter out;
    out.tag("CNXS");
    out.u32(kMachineStateVersion);
    out.u64(layoutFingerprint());
    out.u32(static_cast<uint32_t>(m_loadedModules.size()));
    for (const auto &m : m_loadedModules)
    {
        out.u32(m.baseAddress);
        out.u64(m.size);
    }
    m_eeScheduler->saveState(out);
    m_memory.saveState(out);
    m_gs.saveState(out);
    out.tag("VU0S");
    m_vu0.saveState(out);
    out.tag("VU1S");
    m_vu1.saveState(out);
    out.tag("CPUS");
    out.pod(m_cpuContext);
    ps2_stubs::cdSaveState(out);
    out.tag("HEAP");
    {
        std::lock_guard lock(m_guestHeapMutex);
        out.u32(static_cast<uint32_t>(m_guestHeapBlocks.size()));
        for (const auto &b : m_guestHeapBlocks)
            out.pod(b);
        out.u32(m_guestHeapBase);
        out.u32(m_guestHeapEnd);
        out.u32(m_guestHeapLimit);
        out.u32(m_guestHeapSuggestedBase);
        out.flag(m_guestHeapConfigured);
    }
    {
        std::lock_guard lock(m_asyncCallbackStackMutex);
        out.u32(m_asyncCallbackStackFloor);
        out.u32(m_asyncCallbackStackTop);
    }
    out.tag("KERN");
    {
        std::lock_guard lock(m_eeKernelStateMutex);
        std::vector<int> ids;
        for (const auto &kv : m_eeExitHandlers)
            ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());
        out.u32(static_cast<uint32_t>(ids.size()));
        for (int id : ids)
        {
            const auto &v = m_eeExitHandlers.at(id);
            out.i32(id);
            out.u32(static_cast<uint32_t>(v.size()));
            for (const auto &r : v)
                out.pod(r);
        }
        std::vector<std::pair<uint32_t, uint32_t>> overrides(m_eeSyscallOverrides.begin(), m_eeSyscallOverrides.end());
        std::sort(overrides.begin(), overrides.end());
        out.u32(static_cast<uint32_t>(overrides.size()));
        for (const auto &[k, v] : overrides)
        {
            out.u32(k);
            out.u32(v);
        }
        std::vector<uint32_t> mirrors(m_eeSyscallMirrorAddresses.begin(), m_eeSyscallMirrorAddresses.end());
        std::sort(mirrors.begin(), mirrors.end());
        out.u32(static_cast<uint32_t>(mirrors.size()));
        for (uint32_t a : mirrors)
            out.u32(a);
    }
    out.tag("IOPB");
    out.flag(m_iopSubsystem != nullptr);
    if (m_iopSubsystem)
        out.blob(m_iopSubsystem->saveState());
    out.tag("CNXE");
    bytes = std::move(out.bytes);
    return true;
}

bool PS2Runtime::loadMachineState(const uint8_t *data, size_t size, std::string &error)
{
    ps2state::StateReader in(data, size);
    // Header checks first: nothing is touched until the state is known to be for this game and
    // this layout, and the machine is at a safe point.
    in.tag("CNXS");
    if (in.ok() && in.u32() != kMachineStateVersion)
        in.fail("state version");
    if (in.ok() && in.u64() != layoutFingerprint())
        in.fail("state from an incompatible build");
    const uint32_t modules = in.u32();
    if (in.ok() && modules != m_loadedModules.size())
        in.fail("different game modules");
    for (uint32_t i = 0; i < modules && in.ok(); ++i)
    {
        const uint32_t base = in.u32();
        const uint64_t msize = in.u64();
        if (in.ok() && (base != m_loadedModules[i].baseAddress || msize != m_loadedModules[i].size))
            in.fail("different game executable");
    }
    std::string why;
    if (in.ok() && !machineSafeForState(why))
        in.fail("not at a safe point: " + why);
    if (!in.ok())
    {
        error = in.error();
        return false;
    }
    // From here a failure leaves a broken machine.
    m_eeScheduler->loadState(in);
    m_memory.loadState(in);
    m_gs.loadState(in);
    in.tag("VU0S");
    m_vu0.loadState(in);
    in.tag("VU1S");
    m_vu1.loadState(in);
    in.tag("CPUS");
    m_cpuContext = in.pod<R5900Context>();
    ps2_stubs::cdLoadState(in);
    in.tag("HEAP");
    {
        std::lock_guard lock(m_guestHeapMutex);
        const uint32_t n = in.u32();
        if (n > (1u << 22))
            in.fail("bad heap size");
        m_guestHeapBlocks.clear();
        for (uint32_t i = 0; i < n && in.ok(); ++i)
            m_guestHeapBlocks.push_back(in.pod<GuestHeapBlock>());
        m_guestHeapBase = in.u32();
        m_guestHeapEnd = in.u32();
        m_guestHeapLimit = in.u32();
        m_guestHeapSuggestedBase = in.u32();
        m_guestHeapConfigured = in.flag();
    }
    {
        std::lock_guard lock(m_asyncCallbackStackMutex);
        m_asyncCallbackStackFloor = in.u32();
        m_asyncCallbackStackTop = in.u32();
    }
    in.tag("KERN");
    {
        std::lock_guard lock(m_eeKernelStateMutex);
        m_eeExitHandlers.clear();
        const uint32_t threads = in.u32();
        if (threads > 65536u)
            in.fail("bad exit handler count");
        for (uint32_t i = 0; i < threads && in.ok(); ++i)
        {
            const int id = in.i32();
            const uint32_t n = in.u32();
            if (n > 4096u)
            {
                in.fail("bad exit handler list");
                break;
            }
            auto &v = m_eeExitHandlers[id];
            for (uint32_t k = 0; k < n && in.ok(); ++k)
                v.push_back(in.pod<EeExitHandlerRegistration>());
        }
        m_eeSyscallOverrides.clear();
        const uint32_t o = in.u32();
        if (o > 65536u)
            in.fail("bad syscall override count");
        for (uint32_t i = 0; i < o && in.ok(); ++i)
        {
            const uint32_t k = in.u32();
            m_eeSyscallOverrides[k] = in.u32();
        }
        m_eeSyscallMirrorAddresses.clear();
        const uint32_t m = in.u32();
        if (m > 65536u)
            in.fail("bad syscall mirror count");
        for (uint32_t i = 0; i < m && in.ok(); ++i)
            m_eeSyscallMirrorAddresses.insert(in.u32());
    }
    in.tag("IOPB");
    const bool hadIop = in.flag();
    if (in.ok() && hadIop != (m_iopSubsystem != nullptr))
        in.fail("IOP presence differs");
    if (in.ok() && hadIop)
    {
        std::vector<uint8_t> iop;
        in.blob(iop);
        std::string iopError;
        if (in.ok() && !m_iopSubsystem->loadState(iop.data(), iop.size(), iopError))
            in.fail("IOP: " + iopError);
    }
    in.tag("CNXE");
    if (!in.ok())
        error = in.error();
    return in.ok();
}
