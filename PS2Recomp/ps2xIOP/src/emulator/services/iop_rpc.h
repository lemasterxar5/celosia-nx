#pragma once

#include <type_traits>
#include <vector>
#include <string>
#include <map>
#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    class IopRpcBridge
    {
    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

        // Save states (v94).
        template <typename W>
        void saveState(W &out) const
        {
            std::map<uint32_t, RpcServer> sorted(m_servers.begin(), m_servers.end());
            out.u32(static_cast<uint32_t>(sorted.size()));
            for (const auto &kv : sorted) { out.pod(kv.first); out.pod(kv.second); }
            out.u32(m_nextDmaId); out.flag(m_sifInitialized);
        }
        template <typename R>
        bool loadState(R &in)
        {
            {
                m_servers.clear();
                const uint32_t n = in.u32();
                if (n > 65536u) { in.fail("bad IOP table"); return false; }
                for (uint32_t i = 0; i < n && in.ok(); ++i) {
                    const auto key = in.template pod<typename std::decay_t<decltype(m_servers)>::key_type>();
                    m_servers[key] = in.template pod<typename std::decay_t<decltype(m_servers)>::mapped_type>();
                }
            }
            m_nextDmaId = in.u32(); m_sifInitialized = in.flag();
            return in.ok();
        }

    private:
        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
    };
}
