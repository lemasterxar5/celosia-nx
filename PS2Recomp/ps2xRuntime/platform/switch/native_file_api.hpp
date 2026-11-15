#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include "native_files.hpp"
namespace celosia::native {
class NativeFiles;
class DataMemory;
// Read-only native source descriptors; no disc/device/sector objects. Files and
// data must outlive this owner. Calls serialize descriptor/cursor operations;
// caller coordinates destination data access and destruction with other work.
class NativeFileApi final {
public:
    static constexpr unsigned MaxLive=128;
    static constexpr uint32_t ReadChunk=16u*1024u;
    NativeFileApi(NativeFiles &files,DataMemory &data);
    ~NativeFileApi();
    NativeFileApi(const NativeFileApi &)=delete;
    NativeFileApi &operator=(const NativeFileApi &)=delete;
    int32_t open(std::string_view path);
    int32_t close(int32_t descriptor);
    std::optional<uint64_t> size(int32_t descriptor) const;
    std::optional<NativeFiles::Metadata> stat(std::string_view path) const;
    // origin: 0=begin, 1=current, 2=end. No cursor above INT64_MAX or below0.
    std::optional<uint64_t> seek(int32_t descriptor,int64_t offset,int origin);
    // Validates the WHOLE destination before I/O. Only bounded main-data
    // aliases are accepted. Error after prior copied chunks returns their byte
    // count and advances only by that count; failing staged data is discarded.
    // A valid descriptor with zero bytes returns0 without dereferencing data.
    int32_t read(int32_t descriptor,uint32_t destination,uint32_t bytes);
    // Closes only tokens opened here. External NativeAssets tokens survive.
    // IDs never repeat, including across reset; stale descriptors stay stale.
    bool reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
