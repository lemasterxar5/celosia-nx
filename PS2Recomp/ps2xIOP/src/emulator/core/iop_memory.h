#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "iop_spu2.h"
#include "native/audio_ram.h"

namespace ps2x::iop::detail
{
    class IopMemory : public AudioRam
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        // Modules load from 0x10000 up to HeapBase; the rest is the sysmem heap.
        static constexpr uint32_t HeapBase = 0x00080000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };

        IopMemory();

        void reset();

        [[nodiscard]] uint8_t read8(uint32_t address) const;
        [[nodiscard]] uint16_t read16(uint32_t address) const;
        [[nodiscard]] uint32_t read32(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const override;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size) override;
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const override;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }

        [[nodiscard]] static uint32_t physicalAddress(uint32_t address) noexcept;

        [[nodiscard]] Spu2 &spu2() noexcept { return m_spu2; }
        // Save states (v94): IOP RAM and its ownership map, scratchpad, hardware registers, the
        // kernel heap, the interrupt controller, a DMA start in progress, and the SPU2.
        template <typename W>
        void saveState(W &out) const
        {
            out.raw(m_ram.data(), m_ram.size());
            out.raw(m_owned.data(), m_owned.size());
            out.raw(m_scratch.data(), m_scratch.size());
            std::vector<std::pair<uint32_t, uint32_t>> hw(m_hardware.begin(), m_hardware.end());
            std::sort(hw.begin(), hw.end());
            out.u32(static_cast<uint32_t>(hw.size()));
            for (const auto &kv : hw) { out.u32(kv.first); out.u32(kv.second); }
            out.u32(static_cast<uint32_t>(m_allocations.size()));
            for (const auto &al : m_allocations) out.pod(al);
            out.u32(m_heapCursor); out.u32(m_interruptStatus); out.u32(m_interruptMask); out.u32(m_interruptControl);
            out.flag(m_dmaStart.has_value());
            if (m_dmaStart) out.pod(*m_dmaStart);
            m_spu2.saveState(out);
        }
        template <typename R>
        bool loadState(R &in)
        {
            in.raw(m_ram.data(), m_ram.size());
            in.raw(m_owned.data(), m_owned.size());
            in.raw(m_scratch.data(), m_scratch.size());
            uint32_t n = in.u32();
            if (n > (1u << 20)) { in.fail("bad IOP register count"); return false; }
            m_hardware.clear();
            for (uint32_t i = 0; i < n && in.ok(); ++i) { const uint32_t k = in.u32(); m_hardware[k] = in.u32(); }
            n = in.u32();
            if (n > (1u << 20)) { in.fail("bad IOP allocation count"); return false; }
            m_allocations.clear();
            for (uint32_t i = 0; i < n && in.ok(); ++i) m_allocations.push_back(in.template pod<Allocation>());
            m_heapCursor = in.u32(); m_interruptStatus = in.u32(); m_interruptMask = in.u32(); m_interruptControl = in.u32();
            m_dmaStart.reset();
            if (in.flag()) m_dmaStart = in.template pod<DmaStart>();
            return m_spu2.loadState(in) && in.ok();
        }

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        uint32_t m_heapCursor = HeapBase;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        mutable Spu2 m_spu2;
    };
}
