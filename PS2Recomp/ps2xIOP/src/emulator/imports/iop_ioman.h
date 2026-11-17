#pragma once

#include <type_traits>
#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        explicit IopIoman(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

        // Save states (v94).
        template <typename W>
        void saveState(W &out) const
        {
            out.u32(static_cast<uint32_t>(m_devices.size()));
            for (const auto &d : m_devices)
            {
                out.u32(d.address); out.u32(d.gp);
                out.u32(static_cast<uint32_t>(d.name.size())); out.raw(d.name.data(), d.name.size());
            }
        }
        template <typename R>
        bool loadState(R &in)
        {
            const uint32_t count = in.u32();
            if (count > 256u) { in.fail("bad device count"); return false; }
            m_devices.clear();
            for (uint32_t k = 0; k < count && in.ok(); ++k)
            {
                Device d; d.address = in.u32(); d.gp = in.u32();
                {
                    const uint32_t len = in.u32();
                    if (len > 4096u) { in.fail("bad name"); return false; }
                    d.name.assign(len, 0); in.raw(d.name.data(), len);
                }
                m_devices.push_back(std::move(d));
            }
            return in.ok();
        }

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        IopMemory &m_memory;
        std::vector<Device> m_devices;
    };
}
