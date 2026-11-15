#pragma once
#include "native_clock.hpp"
#include <cstdint>
struct R5900Context;
namespace celosia::native {
class DataMemory;
class SemaphoreSet;
// Kernel library services implemented with native synchronization and elapsed
// OS time. No interrupt controller, hardware timer or register state is modeled.
// The caller owns the game execution gate; data and semaphores outlive this owner.
class NativeKernelServices final {
public:
    NativeKernelServices(DataMemory &data,SemaphoreSet &semaphores);
    NativeKernelServices(DataMemory &data,SemaphoreSet &semaphores,ClockSource source);
    ~NativeKernelServices();
    bool handle(uint32_t address,R5900Context &context);
    void reset();
private:
    DataMemory &data;
    SemaphoreSet &semaphores;
    SystemClock clock;
    int32_t semaphore=-1;
    uint32_t externalSlot=0;
    void initialize();
};
}
