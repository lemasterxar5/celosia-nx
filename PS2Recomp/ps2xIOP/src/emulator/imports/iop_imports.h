#pragma once

#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    struct IopImportCall
    {
        std::string library;
        uint16_t ordinal = 0;
        uint16_t version = 0;
    };

    class IopImportRegistry
    {
    public:
        explicit IopImportRegistry(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] std::optional<IopImportCall> decode(uint32_t pc) const;
        [[nodiscard]] bool registerExportTable(uint32_t address);
        [[nodiscard]] bool releaseExportTable(uint32_t address);
        [[nodiscard]] uint32_t findTable(std::string_view library, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] uint32_t resolve(std::string_view library, uint16_t ordinal, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] int32_t setRebootTimeLibraryHandlingMode(uint32_t address, uint32_t mode);
        void eraseRange(uint32_t base, uint32_t size);

        // Save states (v94).
        template <typename W>
        void saveState(W &out) const
        {
            out.u32(static_cast<uint32_t>(m_libraries.size()));
            for (const auto &kv : m_libraries)
            {
                out.u32(kv.first); out.u32(kv.second.tableAddress); out.pod(kv.second.version);
                out.u32(static_cast<uint32_t>(kv.second.name.size())); out.raw(kv.second.name.data(), kv.second.name.size());
                out.u32(static_cast<uint32_t>(kv.second.functions.size()));
                for (uint32_t f : kv.second.functions) out.u32(f);
            }
        }
        template <typename R>
        bool loadState(R &in)
        {
            const uint32_t count = in.u32();
            if (count > 4096u) { in.fail("bad library count"); return false; }
            m_libraries.clear();
            for (uint32_t k = 0; k < count && in.ok(); ++k)
            {
                const uint32_t key = in.u32();
                ExportLibrary lib; lib.tableAddress = in.u32(); lib.version = in.template pod<uint16_t>();
                {
                    const uint32_t len = in.u32();
                    if (len > 4096u) { in.fail("bad name"); return false; }
                    lib.name.assign(len, 0); in.raw(lib.name.data(), len);
                }
                const uint32_t fn = in.u32();
                if (fn > 65536u) { in.fail("bad export count"); return false; }
                lib.functions.assign(fn, 0);
                for (uint32_t f = 0; f < fn && in.ok(); ++f) lib.functions[f] = in.u32();
                m_libraries.emplace(key, std::move(lib));
            }
            return in.ok();
        }

    private:
        struct ExportLibrary
        {
            uint32_t tableAddress = 0;
            uint16_t version = 0;
            std::string name;
            std::vector<uint32_t> functions;
        };

        [[nodiscard]] const ExportLibrary *findLibrary(std::string_view name, std::optional<uint16_t> version) const;

        IopMemory &m_memory;
        std::map<uint32_t, ExportLibrary> m_libraries;
    };
}
