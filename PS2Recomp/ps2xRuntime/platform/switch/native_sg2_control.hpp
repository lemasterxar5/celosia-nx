#pragma once
#include <array>
#include <cstdint>
struct R5900Context;
namespace celosia::native {
class DataMemory;
class NativeServices;
// Ordinary sound command queues submitted directly to the native mixer. No
// SIF client/server, processor, interrupt or transfer device exists here.
// Caller owns the game execution gate; both dependencies outlive this object.
class NativeSg2Control final {
public:
    NativeSg2Control(DataMemory &data,NativeServices &services):data(data),services(services) {}
    bool handle(uint32_t address,R5900Context &context);
    void reset() noexcept {initialized=false;failed=false;}
private:
    using Command=std::array<uint32_t,4>;
    struct Plan;
    DataMemory &data;
    NativeServices &services;
    std::array<Command,512> staged{};
    bool initialized=false,failed=false;
    void requireReady() const;
    void initialize();
    uint32_t enqueue(const R5900Context &context,bool transfer);
    Plan stage();
    void commit(const Plan &plan,const std::array<uint8_t,400> &reply);
    void tick();
    uint32_t get(uint32_t address,uint32_t index);
};
}
