#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include <atomic>
#include <stdexcept>
#ifdef __SWITCH__
extern "C" {
#include <switch/arm/cache.h>
}
#endif

namespace ps2_syscalls {
void FlushCache(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    if(!runtime || !context || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native cache: invalid data/context");
    // At the verified loader boundary, publish checked module DATA for already
    // compiled ARM functions. Source instruction bytes are never executable.
    const auto mode=getRegU32(context,4);
    if(mode==2) runtime->services().library().publishModule(*context);
    else if(mode!=0) throw std::runtime_error("Native cache: unsupported instruction/invalidate mode");
    std::atomic_thread_fence(std::memory_order_seq_cst);
#ifdef __SWITCH__
    for(const auto buffer:runtime->memory().nativeDataBuffers())
        armDCacheClean(buffer.data(),buffer.size());
#endif
    std::atomic_thread_fence(std::memory_order_seq_cst);
    setReturnU32(context,0);
}
void iFlushCache(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{ FlushCache(ram,context,runtime); }
}
