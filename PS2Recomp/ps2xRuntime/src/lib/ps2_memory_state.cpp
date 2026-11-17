// Save states: the EE's memory and hardware registers (RDRAM, scratchpad, GS local memory, VU
// micro / data memory, the I/O register file, the EE timers, DMA transfers held back by PATH3
// masking, the TLB). Taken after drainForState, when the VU1 worker and the DMA worker are idle.
#include "runtime/ps2_memory.h"
#include "runtime/state_stream.h"

#include <algorithm>

namespace
{
    constexpr uint32_t kStateVersion = 1;

    template <typename Transfer>
    void writeTransfers(ps2state::StateWriter &out, const std::vector<Transfer> &v)
    {
        out.u32(static_cast<uint32_t>(v.size()));
        for (const auto &t : v)
        {
            out.flag(t.fromScratchpad);
            out.u32(t.srcAddr);
            out.u32(t.qwc);
            out.blob(t.chainData);
        }
    }

    template <typename Transfer>
    void readTransfers(ps2state::StateReader &in, std::vector<Transfer> &v)
    {
        v.clear();
        const uint32_t n = in.u32();
        if (n > 65536u)
        {
            in.fail("bad transfer count");
            return;
        }
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            Transfer t;
            t.fromScratchpad = in.flag();
            t.srcAddr = in.u32();
            t.qwc = in.u32();
            in.blob(t.chainData);
            v.push_back(std::move(t));
        }
    }
}

void PS2Memory::drainForState()
{
    if (m_vif1AsyncPending.load(std::memory_order_relaxed))
        vif1AsyncJoin(kJoinOther);
    if (m_mtvuOutstanding.load(std::memory_order_acquire) != 0u && !onVu1Worker())
        mtvuSyncSlow();
}

bool PS2Memory::quietForState(std::string &why) const
{
    if (m_vif1AsyncPending.load(std::memory_order_acquire))
    {
        why = "the DMA worker still walks a display list";
        return false;
    }
    if (m_mtvuOutstanding.load(std::memory_order_acquire) != 0u)
    {
        why = "the VU1 worker still has work";
        return false;
    }
    return true;
}

void PS2Memory::saveState(ps2state::StateWriter &out) const
{
    out.tag("MEMS");
    out.u32(kStateVersion);
    out.raw(m_rdram, PS2_RAM_SIZE);
    out.raw(m_scratchpad, PS2_SCRATCHPAD_SIZE);
    out.raw(m_gsVRAM, PS2_GS_VRAM_SIZE);
    out.raw(m_vu0Code, PS2_VU0_CODE_SIZE);
    out.raw(m_vu0Data, PS2_VU0_DATA_SIZE);
    out.raw(m_vu1Code, PS2_VU1_CODE_SIZE);
    out.raw(m_vu1Data, PS2_VU1_DATA_SIZE);
    {
        std::vector<std::pair<uint32_t, uint32_t>> regs(m_ioRegisters.begin(), m_ioRegisters.end());
        std::sort(regs.begin(), regs.end());
        out.u32(static_cast<uint32_t>(regs.size()));
        for (const auto &[a, v] : regs)
        {
            out.u32(a);
            out.u32(v);
        }
    }
    out.u64(m_timerDeferred);
    out.u64(m_timerBudget);
    out.u32(m_timerIrqLatched);
    for (const auto &t : m_eeTimers)
        out.pod(t);
    out.u32(static_cast<uint32_t>(m_tlbEntries.size()));
    for (const auto &e : m_tlbEntries)
        out.pod(e);
    out.flag(m_seenGifCopy);
    out.flag(m_gifScanned);
    out.flag(m_path3Masked);
    out.u32(m_vif1PendingPath2ImageQwc);
    out.flag(m_vif1PendingPath2DirectHl);
    out.u32(static_cast<uint32_t>(m_path3MaskedFifo.size()));
    for (const auto &packet : m_path3MaskedFifo)
        out.blob(packet);
    writeTransfers(out, m_pendingGifTransfers);
    writeTransfers(out, m_pendingVif0Transfers);
    writeTransfers(out, m_pendingVif1Transfers);
    {
        std::lock_guard lock(const_cast<std::mutex &>(m_completedDmacMutex));
        out.u32(static_cast<uint32_t>(m_completedDmacCauses.size()));
        for (uint32_t c : m_completedDmacCauses)
            out.u32(c);
    }
    out.u32(m_mtvuFbrst);
    out.flag(m_mtvuDeferredGif);
    out.flag(m_mtvuDeferredVif1);
    out.tag("MEME");
}

bool PS2Memory::loadState(ps2state::StateReader &in)
{
    if (!in.tag("MEMS"))
        return false;
    if (in.u32() != kStateVersion)
    {
        in.fail("memory state version");
        return false;
    }
    // Read into place: a failure part way leaves the machine to be thrown away (the caller quits
    // the game rather than run on half a state).
    in.raw(m_rdram, PS2_RAM_SIZE);
    in.raw(m_scratchpad, PS2_SCRATCHPAD_SIZE);
    in.raw(m_gsVRAM, PS2_GS_VRAM_SIZE);
    in.raw(m_vu0Code, PS2_VU0_CODE_SIZE);
    in.raw(m_vu0Data, PS2_VU0_DATA_SIZE);
    in.raw(m_vu1Code, PS2_VU1_CODE_SIZE);
    in.raw(m_vu1Data, PS2_VU1_DATA_SIZE);
    {
        const uint32_t n = in.u32();
        if (n > 1u << 20)
            in.fail("bad register count");
        m_ioRegisters.clear();
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            const uint32_t a = in.u32();
            m_ioRegisters[a] = in.u32();
        }
    }
    m_timerDeferred = in.u64();
    m_timerBudget = in.u64();
    m_timerIrqLatched = in.u32();
    for (auto &t : m_eeTimers)
        t = in.pod<EeTimer>();
    {
        const uint32_t n = in.u32();
        if (n > 4096u)
            in.fail("bad TLB size");
        m_tlbEntries.clear();
        for (uint32_t i = 0; i < n && in.ok(); ++i)
            m_tlbEntries.push_back(in.pod<TLBEntry>());
    }
    m_seenGifCopy = in.flag();
    m_gifScanned = in.flag();
    m_path3Masked = in.flag();
    m_vif1PendingPath2ImageQwc = in.u32();
    m_vif1PendingPath2DirectHl = in.flag();
    {
        const uint32_t n = in.u32();
        if (n > 65536u)
            in.fail("bad PATH3 FIFO size");
        m_path3MaskedFifo.clear();
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            std::vector<uint8_t> packet;
            in.blob(packet);
            m_path3MaskedFifo.push_back(std::move(packet));
        }
    }
    readTransfers(in, m_pendingGifTransfers);
    readTransfers(in, m_pendingVif0Transfers);
    readTransfers(in, m_pendingVif1Transfers);
    {
        const uint32_t n = in.u32();
        if (n > 65536u)
            in.fail("bad DMAC cause count");
        std::lock_guard lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
        for (uint32_t i = 0; i < n && in.ok(); ++i)
            m_completedDmacCauses.push_back(in.u32());
    }
    m_mtvuFbrst = in.u32();
    m_mtvuDeferredGif = in.flag();
    m_mtvuDeferredVif1 = in.flag();
    in.tag("MEME");
    // Every generated code region and VU program cache sees new contents.
    markVU0CodeModified();
    markVU1CodeModified();
    return in.ok();
}
