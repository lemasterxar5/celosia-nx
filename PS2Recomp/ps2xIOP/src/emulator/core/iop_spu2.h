#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2: two cores of 24 ADPCM voices sharing 2 MiB of sound RAM, mixed at
    // 48 kHz. Implements the register file seen by LIBSD, IOP DMA (manual and
    // auto-DMA streaming), ADSR envelopes, voice/core volumes and the IRQA
    // interrupt. Reverb and noise are not emulated yet.
    class Spu2
    {
    public:
        static constexpr uint32_t Base = 0x1F900000u;
        static constexpr uint32_t RegisterBytes = 0x800u;
        static constexpr uint32_t CyclesPerSample = 768u; // 36.864 MHz / 48 kHz
        static constexpr uint32_t RamWords = 1u << 20u;   // 2 MiB

        Spu2();

        void reset();

        [[nodiscard]] static bool isRegister(uint32_t phys) noexcept
        {
            return phys >= Base && phys < Base + RegisterBytes;
        }
        [[nodiscard]] uint16_t read16(uint32_t phys);
        void write16(uint32_t phys, uint16_t value);

        // IOP DMA channel 4 (core 0) / 7 (core 1). Returns the cycles until the
        // transfer should signal completion.
        uint64_t dmaWrite(int core, const uint8_t *source, uint32_t bytes);
        uint64_t dmaRead(int core, uint8_t *destination, uint32_t bytes);

        // Produces output samples up to the given IOP cycle.
        void advanceTo(uint64_t iopCycle);
        // True once per raised SPU2 interrupt (IOP interrupt line 9).
        [[nodiscard]] bool takeIrq() noexcept;

        // Interleaved stereo frames for the host audio device (any thread).
        size_t readOutput(int16_t *destination, size_t frames) noexcept;
        // Save states (v94): sound RAM, registers, both cores (voices, envelopes, reverb, the
        // auto-DMA input), the IRQ and the clock. The output ring is not state: emptied at load.
        template <typename W>
        void saveState(W &out) const
        {
            out.raw(m_ram.data(), m_ram.size() * sizeof(uint16_t));
            out.pod(m_regs);
            for (const Core &c : m_cores)
            {
                out.raw(&c, offsetof(Core, input));
                out.u32(static_cast<uint32_t>(c.input.size()));
                for (const auto &frame : c.input)
                    out.pod(frame);
            }
            out.pod(m_irqInfo); out.pod(m_irqPending); out.pod(m_cycle); out.pod(m_cycleValid);
        }
        template <typename R>
        bool loadState(R &in)
        {
            in.raw(m_ram.data(), m_ram.size() * sizeof(uint16_t));
            m_regs = in.template pod<decltype(m_regs)>();
            for (Core &c : m_cores)
            {
                in.raw(&c, offsetof(Core, input));
                const uint32_t n = in.u32();
                if (n > (1u << 20))
                {
                    in.fail("bad SPU2 input size");
                    return false;
                }
                c.input.clear();
                for (uint32_t i = 0; i < n && in.ok(); ++i)
                    c.input.push_back(in.template pod<std::array<int16_t, 2>>());
            }
            m_irqInfo = in.template pod<uint16_t>(); m_irqPending = in.template pod<bool>();
            m_cycle = in.template pod<uint64_t>(); m_cycleValid = in.template pod<bool>();
            m_ringRead.store(m_ringWrite.load(std::memory_order_acquire), std::memory_order_release);
            m_outputStarted = false;
            return in.ok();
        }

    private:
        enum class Phase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release
        };

        struct Voice
        {
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            int16_t volXL = 0, volXR = 0;
            uint32_t ssa = 0, lsax = 0, nax = 0;
            Phase phase = Phase::Off;
            int32_t level = 0;
            uint32_t envCounter = 0;
            uint32_t counter = 0;
            int32_t prev1 = 0, prev2 = 0;
            std::array<int16_t, 28> block{};
            uint32_t blockIndex = 28;
            uint8_t blockFlags = 0;
            uint32_t blockAddress = 0;
            std::array<int16_t, 4> history{};
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint16_t attr = 0, mmix = 0, admas = 0, statx = 0;
            uint32_t irqa = 0, tsa = 0;
            uint32_t vmixl = 0, vmixr = 0, endx = 0;
            uint16_t mvolL = 0, mvolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            // Reverb: 22 address offsets (dAPF1..mRAPF2 in PSX order), 10
            // coefficients (vIIR..vRIN), work area ESA..EEA, output EVOL.
            uint32_t vmixel = 0, vmixer = 0;
            uint16_t evolL = 0, evolR = 0;
            std::array<uint32_t, 22> revAddr{};
            std::array<int16_t, 10> revVol{};
            uint32_t esa = 0, eea = 0, reverbX = 0;
            bool reverbOdd = false;
            int32_t revInL = 0, revInR = 0;
            int32_t revOutL = 0, revOutR = 0, revPrevL = 0, revPrevR = 0;
            std::deque<std::array<int16_t, 2>> input; // auto-DMA stream
        };

        [[nodiscard]] static int32_t fixedVolume(uint16_t value) noexcept;
        void keyOn(int core, uint32_t mask);
        void keyOff(int core, uint32_t mask);
        void checkIrq(uint32_t start, uint32_t words);
        void decodeBlock(Core &core, int coreIndex, Voice &voice);
        int32_t nextVoiceSample(Core &core, int coreIndex, Voice &voice, int voiceIndex);
        void tickEnvelope(Voice &voice);
        void mixSample(int16_t &outL, int16_t &outR);
        void processReverb(Core &core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR);
        void pushOutput(int16_t left, int16_t right) noexcept;

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, RegisterBytes / 2u> m_regs{};
        std::array<Core, 2> m_cores{};
        uint16_t m_irqInfo = 0;
        bool m_irqPending = false;
        uint64_t m_cycle = 0;
        bool m_cycleValid = false;

        static constexpr size_t kRingFrames = 1u << 15u;
        std::vector<int16_t> m_ring;
        std::atomic<size_t> m_ringWrite{0};
        std::atomic<size_t> m_ringRead{0};
        bool m_outputStarted = false; // consumer side only
    };
}
