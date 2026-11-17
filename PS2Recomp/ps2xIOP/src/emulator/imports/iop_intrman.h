#pragma once

#include <type_traits>
#include <vector>
#include <string>
#include <cstdint>
#include <map>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIntrman
    {
    public:
        explicit IopIntrman(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);
        [[nodiscard]] bool dispatchInterrupt(int irq, IopGuestExecutor &executor) const;

        // Save states (v94).
        template <typename W>
        void saveState(W &out) const
        {
            out.u32(static_cast<uint32_t>(m_handlers.size()));
            for (const auto &kv : m_handlers) { out.pod(kv.first); out.pod(kv.second); }
            out.u32(static_cast<uint32_t>(m_enabled.size()));
            for (const auto &kv : m_enabled) { out.pod(kv.first); out.pod(kv.second); }
        }
        template <typename R>
        bool loadState(R &in)
        {
            {
                m_handlers.clear();
                const uint32_t n = in.u32();
                if (n > 65536u) { in.fail("bad IOP table"); return false; }
                for (uint32_t i = 0; i < n && in.ok(); ++i) {
                    const auto key = in.template pod<typename std::decay_t<decltype(m_handlers)>::key_type>();
                    m_handlers[key] = in.template pod<typename std::decay_t<decltype(m_handlers)>::mapped_type>();
                }
            }
            {
                m_enabled.clear();
                const uint32_t n = in.u32();
                if (n > 65536u) { in.fail("bad IOP table"); return false; }
                for (uint32_t i = 0; i < n && in.ok(); ++i) {
                    const auto key = in.template pod<typename std::decay_t<decltype(m_enabled)>::key_type>();
                    m_enabled[key] = in.template pod<typename std::decay_t<decltype(m_enabled)>::mapped_type>();
                }
            }
            return in.ok();
        }

    private:
        struct Handler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
            uint32_t gp = 0u;
        };

        IopMemory &m_memory;
        std::map<int, Handler> m_handlers;
        std::map<int, bool> m_enabled;
    };
}
