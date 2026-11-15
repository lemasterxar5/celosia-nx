#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace celosia::native {
class NativeFiles;
class DataMemory;
struct AssetEntry {
    std::string_view name;
    uint64_t offset,bytes;
};
// Reads extracted native files using their original archive-offset units.
// No optical-drive/device state, timing, registers or IOP behavior is modeled.
// NativeFiles and DataMemory must outlive this owner. The caller coordinates
// DataMemory access with compiled game execution and other producers.
class NativeAssets final {
public:
    static constexpr uint64_t DataUnitBytes=2048;
    static constexpr unsigned MaxEntries=256;
    NativeAssets(NativeFiles &files,DataMemory &memory,std::span<const AssetEntry> entries);
    ~NativeAssets();
    NativeAssets(const NativeAssets &)=delete;
    NativeAssets &operator=(const NativeAssets &)=delete;
    bool initialize();
    bool ready() const;
    // Returned name views belong to this owner and remain valid until its
    // destruction. Initialization verifies every configured native file size.
    std::optional<AssetEntry> search(std::string_view path);
    // Validates the complete request before writing. Actual I/O failure after
    // validation may leave complete earlier chunks in the destination.
    bool readAtDataUnits(uint32_t lba,uint32_t sectors,uint32_t destination);
    std::optional<std::string> lastError() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
