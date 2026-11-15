#include "native_file_api.hpp"
#include "native_files.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace celosia::native {
namespace {
std::optional<uint32_t> mainDestination(uint32_t address,uint32_t bytes)
{
    for(const auto base:{0u,0x20000000u,0x80000000u,0xA0000000u}) {
        if(address<base) continue;
        const auto offset=address-base;
        if(offset<=DataMemory::MainBytes && bytes<=DataMemory::MainBytes-offset) return offset;
    }
    return {};
}
int32_t readResult(uint32_t copied) { return copied?static_cast<int32_t>(copied):-1; }
}
struct NativeFileApi::Impl {
    struct Descriptor {uint64_t token,position;};
    NativeFiles &files;
    DataMemory &data;
    mutable std::mutex mutex;
    std::unordered_map<int32_t,Descriptor> descriptors;
    // Source C stdio owns 0/1/2; never alias an asset with a console sink.
    uint64_t next=3;
    Impl(NativeFiles &source,DataMemory &memory) : files(source),data(memory) {}
};
NativeFileApi::NativeFileApi(NativeFiles &files,DataMemory &data) : impl(std::make_unique<Impl>(files,data)) {}
NativeFileApi::~NativeFileApi()
{
    try { if(!reset()) std::fputs("Native file API: an owned file failed to close\n",stderr); }
    catch(...) { std::fputs("Native file API: descriptor teardown failed\n",stderr); }
}
int32_t NativeFileApi::open(std::string_view path)
{
    auto &s=*impl; std::lock_guard lock(s.mutex);
    if(s.descriptors.size()>=MaxLive || s.next>INT32_MAX) return -1;
    const auto token=s.files.open(path);
    if(!token) return -1;
    const auto descriptor=static_cast<int32_t>(s.next);
    try { s.descriptors.emplace(descriptor,Impl::Descriptor{token,0}); }
    catch(...) { (void)s.files.close(token); throw; }
    ++s.next;
    return descriptor;
}
int32_t NativeFileApi::close(int32_t descriptor)
{
    auto &s=*impl; std::lock_guard lock(s.mutex);
    const auto found=s.descriptors.find(descriptor);
    if(found==s.descriptors.end()) return -1;
    const auto token=found->second.token;
    s.descriptors.erase(found);
    return s.files.close(token)?0:-1;
}
std::optional<uint64_t> NativeFileApi::size(int32_t descriptor) const
{
    auto &s=*impl; std::lock_guard lock(s.mutex);
    const auto found=s.descriptors.find(descriptor);
    return found==s.descriptors.end()?std::nullopt:s.files.size(found->second.token);
}
std::optional<NativeFiles::Metadata> NativeFileApi::stat(std::string_view path) const
{
    return impl->files.stat(path);
}
std::optional<uint64_t> NativeFileApi::seek(int32_t descriptor,int64_t offset,int origin)
{
    auto &s=*impl; std::lock_guard lock(s.mutex);
    const auto found=s.descriptors.find(descriptor);
    if(found==s.descriptors.end()) return {};
    uint64_t base=0;
    if(origin==1) base=found->second.position;
    else if(origin==2) {
        const auto end=s.files.size(found->second.token);
        if(!end) return {};
        base=*end;
    } else if(origin!=0) return {};
    const auto position=static_cast<__int128>(base)+static_cast<__int128>(offset);
    if(position<0 || position>INT64_MAX) return {};
    found->second.position=static_cast<uint64_t>(position);
    return found->second.position;
}
int32_t NativeFileApi::read(int32_t descriptor,uint32_t destination,uint32_t bytes)
{
    auto &s=*impl; std::lock_guard lock(s.mutex);
    const auto found=s.descriptors.find(descriptor);
    if(found==s.descriptors.end()) return -1;
    if(!bytes) return 0;
    const auto offset=mainDestination(destination,bytes);
    if(!offset || !s.data.contains(*offset,bytes)) return -1;
    std::array<uint8_t,ReadChunk> staging;
    auto &file=found->second;
    uint32_t copied=0;
    while(copied<bytes) {
        const auto count=std::min<uint32_t>(bytes-copied,ReadChunk);
        const auto read=s.files.read(file.token,file.position,std::span(staging).first(count));
        if(!read || *read>count || *read>uint64_t(INT64_MAX)-file.position) return readResult(copied);
        if(!*read) break;
        if(!s.data.write(*offset+copied,staging.data(),*read)) return readResult(copied);
        copied+=static_cast<uint32_t>(*read);
        file.position+=*read;
        if(*read<count) break;
    }
    return static_cast<int32_t>(copied);
}
bool NativeFileApi::reset()
{
    auto &s=*impl; std::lock_guard lock(s.mutex);
    bool closed=true;
    for(const auto &[descriptor,file]:s.descriptors) {
        (void)descriptor;
        try { if(!s.files.close(file.token)) closed=false; }
        catch(...) { closed=false; }
    }
    s.descriptors.clear();
    return closed;
}
}
