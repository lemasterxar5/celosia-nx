#include "native_assets.hpp"
#include "native_files.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace celosia::native {
namespace {
std::string relativeName(std::string_view source)
{
    if(source.empty() || source.size()>NativeFiles::PathLimit || source.front()=='/' ||
       source.front()=='\\' || source.find_first_of(":;")!=std::string_view::npos ||
       source.find('\0')!=std::string_view::npos)
        throw std::invalid_argument("Invalid asset metadata name");
    std::string result(source);
    for(auto &value:result) {
        if(value=='\\') value='/';
        else if(value>='a' && value<='z') value=static_cast<char>(value-'a'+'A');
    }
    return result;
}
std::optional<uint32_t> mainOffset(uint32_t address,uint64_t bytes)
{
    for(const uint32_t base:{0u,0x20000000u,0x80000000u,0xA0000000u}) {
        if(address<base) continue;
        const uint64_t offset=uint64_t(address)-base;
        if(offset<DataMemory::MainBytes && bytes<=DataMemory::MainBytes-offset)
            return static_cast<uint32_t>(offset);
    }
    return {};
}
}
struct NativeAssets::Impl {
    struct Record {
        std::string name,path;
        uint64_t offset,bytes,end,token=0;
    };
    NativeFiles &files;
    DataMemory &memory;
    std::vector<Record> records;
    mutable std::mutex mutex;
    std::optional<std::string> error;
    bool validMetadata=false,initialized=false;
    Impl(NativeFiles &source,DataMemory &data,std::span<const AssetEntry> entries)
        : files(source),memory(data)
    {
        if(entries.empty() || entries.size()>MaxEntries) return;
        records.reserve(entries.size());
        try {
            for(const auto &entry:entries) {
                if(entry.offset%DataUnitBytes || entry.bytes>UINT64_MAX-(DataUnitBytes-1)) return;
                const auto rounded=(entry.bytes+DataUnitBytes-1)/DataUnitBytes*DataUnitBytes;
                if(entry.offset>UINT64_MAX-rounded) return;
                const auto end=entry.offset+rounded;
                auto name=relativeName(entry.name);
                auto path=files.translate(name);
                for(const auto &prior:records)
                    if(path==prior.path || (entry.offset<prior.end && prior.offset<end && rounded && prior.bytes)) return;
                records.push_back({std::move(name),std::move(path),entry.offset,entry.bytes,end});
            }
            validMetadata=true;
        } catch(const std::invalid_argument &) { validMetadata=false; }
    }
    bool fail(const char *message) { error=message; return false; }
    void closeFiles() noexcept
    {
        for(auto &record:records) if(record.token) {
            (void)files.close(record.token); record.token=0;
        }
    }
    bool openFiles()
    {
        for(auto &record:records) {
            record.token=files.open(record.path);
            if(!record.token) return fail("Native asset file is missing or cannot be opened");
            if(files.size(record.token)!=record.bytes) return fail("Native asset file size does not match metadata");
        }
        return true;
    }
    Record *findRange(uint64_t start,uint64_t bytes)
    {
        for(auto &record:records)
            if(start>=record.offset && start<record.end && bytes<=record.end-start) return &record;
        return nullptr;
    }
    bool copy(Record &record,uint64_t relative,uint64_t bytes,uint32_t destination)
    {
        const auto actual=relative<record.bytes ? std::min(bytes,record.bytes-relative) : 0;
        std::array<uint8_t,16384> chunk{};
        uint64_t copied=0;
        while(copied<actual) {
            const auto count=static_cast<size_t>(std::min<uint64_t>(chunk.size(),actual-copied));
            if(files.read(record.token,relative+copied,std::span(chunk.data(),count))!=count)
                return fail("Native asset read failed or reached an unexpected EOF");
            if(!memory.write(static_cast<uint32_t>(destination+copied),chunk.data(),count))
                return fail("Native asset validated destination copy failed");
            copied+=count;
        }
        // A request was confined to this one rounded extent. Therefore zeros
        // can only cover the unused tail of its final 2048-byte block.
        if(bytes>actual && !memory.zero(static_cast<uint32_t>(destination+actual),static_cast<size_t>(bytes-actual)))
            return fail("Native asset validated padding failed");
        return true;
    }
};
NativeAssets::NativeAssets(NativeFiles &files,DataMemory &memory,std::span<const AssetEntry> entries)
    : impl(std::make_unique<Impl>(files,memory,entries)) {}
NativeAssets::~NativeAssets() { impl->closeFiles(); }
bool NativeAssets::initialize()
{
    std::lock_guard lock(impl->mutex);
    if(impl->initialized) { impl->error.reset(); return true; }
    if(!impl->validMetadata) return impl->fail("Invalid native asset metadata");
    if(!impl->openFiles()) { impl->closeFiles(); return false; }
    impl->initialized=true; impl->error.reset(); return true;
}
bool NativeAssets::ready() const
{ std::lock_guard lock(impl->mutex); return impl->initialized; }
std::optional<AssetEntry> NativeAssets::search(std::string_view path)
{
    std::lock_guard lock(impl->mutex);
    if(!impl->initialized) { impl->fail("Native assets are not initialized"); return {}; }
    try {
        const auto translated=impl->files.translate(path);
        for(const auto &record:impl->records) if(record.path==translated) {
            impl->error.reset(); return AssetEntry{record.name,record.offset,record.bytes};
        }
        impl->fail("Unknown native asset path");
    } catch(const std::invalid_argument &) { impl->fail("Invalid native asset path"); }
    return {};
}
bool NativeAssets::readAtDataUnits(uint32_t lba,uint32_t sectors,uint32_t destination)
{
    std::lock_guard lock(impl->mutex);
    if(!impl->initialized) return impl->fail("Native assets are not initialized");
    const uint64_t bytes=uint64_t(sectors)*DataUnitBytes,start=uint64_t(lba)*DataUnitBytes;
    const auto offset=mainOffset(destination,bytes);
    if(!sectors || !offset) return impl->fail("Invalid native asset data destination or length");
    auto *record=impl->findRange(start,bytes);
    if(!record) return impl->fail("Native asset range is unknown or crosses a file extent");
    if(impl->files.size(record->token)!=record->bytes) return impl->fail("Native asset file changed size");
    if(!impl->copy(*record,start-record->offset,bytes,*offset)) return false;
    impl->error.reset(); return true;
}
std::optional<std::string> NativeAssets::lastError() const
{ std::lock_guard lock(impl->mutex); return impl->error; }
}
