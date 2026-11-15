#include "native_boot_services.hpp"
#include "native_aot_runtime.hpp"
#include <array>

namespace celosia::native {
bool BootServices::handleSemaphore(uint32_t id,R5900Context &ctx)
{
    const auto argument=getRegU32(&ctx,4);
    const auto resource=static_cast<int32_t>(argument);
    int32_t result;
    switch(id) {
    case 0x40: {
        std::array<uint32_t,6> descriptor;
        if(!argument || !data.read(argument,descriptor.data(),sizeof(descriptor))) result=SemaphoreSet::Error;
        else result=synchronization.create(static_cast<int32_t>(descriptor[2]),static_cast<int32_t>(descriptor[1]),
                                            descriptor[4],descriptor[5]);
        break;
    }
    case 0x41: case uint32_t(-0x49): result=synchronization.erase(resource); break;
    case 0x42: case uint32_t(-0x43): result=synchronization.signal(resource); break;
    case 0x44: result=synchronization.wait(resource); break;
    case 0x45: case uint32_t(-0x46): result=synchronization.poll(resource); break;
    case 0x47: case uint32_t(-0x48): {
        const auto status=synchronization.status(resource);
        if(!status) { result=SemaphoreSet::Unknown; break; }
        const std::array<uint32_t,6> bytes{uint32_t(status->count),uint32_t(status->max),uint32_t(status->initial),
            uint32_t(status->waiters),status->attr,status->option};
        const auto destination=getRegU32(&ctx,5);
        result=destination && data.write(destination,bytes.data(),sizeof(bytes)) ? 0 : SemaphoreSet::Error;
        break;
    }
    default: return false;
    }
    setReturnS32(&ctx,result);
    return true;
}
}
namespace ps2_syscalls {
#define NATIVE_SYNC(name,id) \
void name(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) { runtime->handleSyscall(ram,ctx,id); }
NATIVE_SYNC(CreateSema,0x40)
NATIVE_SYNC(DeleteSema,0x41)
NATIVE_SYNC(iDeleteSema,uint32_t(-0x49))
NATIVE_SYNC(SignalSema,0x42)
NATIVE_SYNC(iSignalSema,uint32_t(-0x43))
NATIVE_SYNC(WaitSema,0x44)
NATIVE_SYNC(PollSema,0x45)
NATIVE_SYNC(iPollSema,uint32_t(-0x46))
NATIVE_SYNC(ReferSemaStatus,0x47)
NATIVE_SYNC(iReferSemaStatus,uint32_t(-0x48))
#undef NATIVE_SYNC
}
