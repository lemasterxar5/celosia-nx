#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <optional>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopCdvd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t reason = 0u;
        };

        IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel);
        ~IopCdvd();

        IopCdvd(const IopCdvd &) = delete;
        IopCdvd &operator=(const IopCdvd &) = delete;

        void reset() noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;
        // Save states (v94): the drive's guest-visible state as bytes (the disc image's index and
        // host handle stay as they are).
        [[nodiscard]] std::vector<uint8_t> saveStateBytes() const;
        bool loadStateBytes(const uint8_t *data, size_t size);

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
