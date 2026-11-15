#include "native_pad_api.hpp"
#include "native_input.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <mutex>
#include <stdexcept>
namespace celosia::native {
namespace {
bool mainRange(uint32_t address,size_t bytes)
{ return address && address<DataMemory::MainBytes && bytes<=DataMemory::MainBytes-address; }
std::array<uint8_t,NativePadApi::PacketBytes> wirePacket(const NativePadSnapshot &input)
{
    std::array<uint8_t,NativePadApi::PacketBytes> bytes{};
    bytes[0]=static_cast<uint8_t>(input.sourceButtonsActiveLow);
    bytes[1]=static_cast<uint8_t>(input.sourceButtonsActiveLow>>8);
    bytes[2]=input.rightX; bytes[3]=input.rightY; bytes[4]=input.leftX; bytes[5]=input.leftY;
    std::copy(input.pressures.begin(),input.pressures.end(),bytes.begin()+6);
    return bytes;
}
}
struct NativePadApi::Impl {
    struct Socket { int32_t id=-1; uint32_t port=0; };
    NativeInput &input;
    DataMemory &data;
    mutable std::mutex mutex;
    std::array<Socket,MaxSockets> sockets{};
    uint64_t next=0;
    bool ready=false;
    Statistics counts;
    Impl(NativeInput &source,DataMemory &memory):input(source),data(memory) {}
    Socket *find(int32_t id)
    {
        if(!ready || id<0) return nullptr;
        const auto found=std::find_if(sockets.begin(),sockets.end(),[&](const Socket &s){ return s.id==id; });
        return found==sockets.end() ? nullptr : &*found;
    }
    NativePadSnapshot sample(const Socket &socket)
    {
        if(socket.port!=0) return {};
        ++counts.inputPolls;
        return input.poll();
    }
};
NativePadApi::NativePadApi(NativeInput &input,DataMemory &data):impl(std::make_unique<Impl>(input,data)) {}
NativePadApi::~NativePadApi()=default;
int32_t NativePadApi::initialize(uint32_t flags)
{
    std::lock_guard lock(impl->mutex);
    if(flags) return -1;
    if(impl->ready) return 1;
    if(!impl->input.initialize()) return -1;
    impl->ready=true;
    return 1;
}
int32_t NativePadApi::end()
{
    std::lock_guard lock(impl->mutex);
    for(auto &socket:impl->sockets) {
        if(socket.id>=0) ++impl->counts.closed;
        socket.id=-1;
    }
    impl->ready=false;
    return 1;
}
int32_t NativePadApi::createSocket(uint32_t parameterAddress,uint32_t workAddress)
{
    std::lock_guard lock(impl->mutex);
    std::array<uint32_t,3> descriptor{};
    if(!impl->ready || !mainRange(parameterAddress,sizeof(descriptor)) ||
       (workAddress && !mainRange(workAddress,WorkBytes)) ||
       !impl->data.read(parameterAddress,descriptor.data(),sizeof(descriptor)) ||
       descriptor[0]!=2 || descriptor[1]>1 || descriptor[2]!=0 || impl->next>INT32_MAX) return -1;
    auto free=std::find_if(impl->sockets.begin(),impl->sockets.end(),[](const Impl::Socket &s){ return s.id<0; });
    if(free==impl->sockets.end()) return -1;
    free->id=static_cast<int32_t>(impl->next++); free->port=descriptor[1];
    ++impl->counts.created;
    return free->id;
}
int32_t NativePadApi::deleteSocket(int32_t id)
{
    std::lock_guard lock(impl->mutex);
    auto *socket=impl->find(id);
    if(!socket) return -1;
    socket->id=-1; ++impl->counts.closed;
    return 1;
}
int32_t NativePadApi::getState(int32_t id)
{
    std::lock_guard lock(impl->mutex);
    const auto *socket=impl->find(id);
    return socket && impl->sample(*socket).connected ? 1 : 0;
}
int32_t NativePadApi::getButtonProfile(int32_t id,uint32_t address)
{
    std::lock_guard lock(impl->mutex);
    const auto *socket=impl->find(id);
    if(!socket || !mainRange(address,4) || !impl->sample(*socket).connected) return -1;
    constexpr std::array<uint8_t,4> profile{255,255,255,255};
    if(!impl->data.write(address,profile.data(),profile.size())) throw std::logic_error("Native pad profile bounds invariant");
    return 4;
}
int32_t NativePadApi::read(int32_t id,uint32_t address)
{
    std::lock_guard lock(impl->mutex);
    const auto *socket=impl->find(id);
    if(!socket || !mainRange(address,PacketBytes)) return -1;
    ++impl->counts.readAttempts;
    const auto snapshot=impl->sample(*socket);
    const auto bytes=wirePacket(snapshot);
    if(!impl->data.write(address,bytes.data(),bytes.size())) throw std::logic_error("Native pad read bounds invariant");
    if(!snapshot.connected) return -1;
    ++impl->counts.successfulReads;
    return PacketBytes;
}
NativePadApi::Statistics NativePadApi::statistics() const
{ std::lock_guard lock(impl->mutex); return impl->counts; }
}
