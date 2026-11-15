#pragma once
#include <cstdint>
#include <memory>
namespace celosia::native {
class NativeInput;
class DataMemory;
// Native single-player HID translated to the SCUS libpad2 data ABI. Socket
// records are ordinary application handles, not SIO2/IOP controller devices.
class NativePadApi final {
public:
    static constexpr unsigned MaxSockets=8,PacketBytes=18,WorkBytes=0x140;
    struct Statistics {
        uint64_t created=0,closed=0,inputPolls=0,readAttempts=0,successfulReads=0;
    };
    NativePadApi(NativeInput &input,DataMemory &data);
    ~NativePadApi();
    NativePadApi(const NativePadApi &)=delete;
    NativePadApi &operator=(const NativePadApi &)=delete;
    int32_t initialize(uint32_t flags=0);
    int32_t end();
    // SCUS descriptor: type2, port0/1, slot0. Optional work memory is validated
    // as a bounded SCUS data block; no fabricated driver state is written there.
    int32_t createSocket(uint32_t parameterAddress,uint32_t workAddress=0);
    int32_t deleteSocket(int32_t socket);
    int32_t getState(int32_t socket);
    int32_t getButtonProfile(int32_t socket,uint32_t outputAddress);
    // Disconnected valid sockets write a neutral packet and return-1.
    // Invalid/closed handles or invalid ranges leave the destination untouched.
    int32_t read(int32_t socket,uint32_t outputAddress);
    Statistics statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
