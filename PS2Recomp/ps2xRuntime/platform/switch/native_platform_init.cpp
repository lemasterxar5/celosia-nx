#include "native_platform_init.hpp"
#include "native_assets.hpp"
#include "native_boot_services.hpp"
#include "native_file_api.hpp"
#include "native_memory.hpp"
#include "native_pad_api.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>

namespace celosia::native {
namespace {
// Verified padsysInit@104DF8: two0x140-byte work blocks, actual socket at
// +0x11C, unpolled source readiness at+0x120. Other work bytes remain untouched.
constexpr std::array<uint32_t,2> PadWork{0x13D840,0x13D980};
void writeWord(DataMemory &memory,uint32_t address,uint32_t value)
{
    if(!memory.write(address,&value,sizeof(value)))
        throw std::out_of_range("Native platform: pad metadata exceeds data bounds");
}
void invalidatePadMetadata(DataMemory &memory)
{
    for(const auto work:PadWork) {
        writeWord(memory,work+0x11C,UINT32_MAX);
        writeWord(memory,work+0x120,0);
    }
}
void reportUnavailable(const PrintSink &sink)
{
    constexpr std::string_view message="Native DBC and USB serial unavailable; native controller input uses libnx HID.\n";
    const auto count=sink ? sink(message) : int(std::fwrite(message.data(),1,message.size(),stdout));
    if(count!=int(message.size()) || (!sink && std::ferror(stdout)))
        throw std::runtime_error("Native platform: diagnostic output failed");
}
struct Descriptor final {
    DataHeap &heap;
    uint32_t address;
    ~Descriptor() { if(address) (void)heap.release(address); }
};
void createPadSockets(DataMemory &memory,BootServices &boot,uint32_t descriptor,
    std::array<int32_t,2> &sockets)
{
    auto &pads=boot.pads();
    if(pads.initialize()!=1) throw std::runtime_error("Native platform: HID initialization failed");
    for(unsigned port=0;port<PadWork.size();++port) {
        const std::array<uint32_t,3> parameter{2,port,0};
        if(!memory.write(descriptor,parameter.data(),sizeof(parameter)))
            throw std::out_of_range("Native platform: descriptor exceeds data bounds");
        sockets[port]=pads.createSocket(descriptor,PadWork[port]);
        if(sockets[port]<0) throw std::runtime_error("Native platform: controller socket creation failed");
    }
}
}
void initializeScusPlatform(DataMemory &memory,BootServices &boot,
    std::function<void()> prepareLifecycle,std::function<void()> synchronizeDisplay,PrintSink log)
{
    if(!prepareLifecycle || !synchronizeDisplay)
        throw std::invalid_argument("Native platform: required lifecycle/display hook missing");
    for(const auto work:PadWork)
        if(!memory.contains(work,NativePadApi::WorkBytes))
            throw std::out_of_range("Native platform: controller work exceeds data bounds");
    Descriptor descriptor{boot.heap(),boot.heap().allocate(12)};
    if(!descriptor.address) throw std::runtime_error("Native platform: controller descriptor allocation failed");
    for(const auto work:PadWork)
        if(descriptor.address<work+NativePadApi::WorkBytes && uint64_t(descriptor.address)+12>work)
            throw std::invalid_argument("Native platform: descriptor overlaps controller work");
    if(!boot.assets().initialize())
        throw std::runtime_error("Native platform: required asset initialization failed");
    boot.services().initialize();
    boot.services().reset();
    invalidatePadMetadata(memory);
    try {
        if(!boot.fileApi().reset()) throw std::runtime_error("Native platform: readonly file reset failed");
        prepareLifecycle();
        synchronizeDisplay();
        for(const auto alias:{"SIO2MAN.IRX","SIO2D.IRX","DS1O_D.IRX"})
            if(boot.services().services().loadModule(alias).moduleId<0)
                throw std::runtime_error("Native platform: required native controller provider unavailable");
        std::array<int32_t,2> sockets{-1,-1};
        createPadSockets(memory,boot,descriptor.address,sockets);
        reportUnavailable(log);
        for(unsigned port=0;port<PadWork.size();++port)
            writeWord(memory,PadWork[port]+0x11C,static_cast<uint32_t>(sockets[port]));
    } catch(...) {
        // Native controller reset closes every socket created by this attempt.
        // The failure propagates; no unimplemented module/RPC is published.
        boot.services().reset();
        invalidatePadMetadata(memory);
        throw;
    }
}
}
