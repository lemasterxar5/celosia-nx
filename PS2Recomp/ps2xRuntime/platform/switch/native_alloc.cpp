#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"

namespace {
celosia::native::DataHeap &heap(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!runtime || !ctx || ram != runtime->memory().mainRam().data())
        throw std::invalid_argument("Native allocator: invalid data context");
    return runtime->services().heap();
}
void release(celosia::native::DataHeap &allocator, uint32_t pointer)
{
    if (!allocator.release(pointer)) throw std::invalid_argument("Native allocator: invalid free");
}
}
namespace ps2_stubs {
void malloc(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).allocate(getRegU32(ctx,4))); }
void calloc(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).calloc(getRegU32(ctx,4),getRegU32(ctx,5))); }
void realloc(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).reallocate(getRegU32(ctx,4),getRegU32(ctx,5))); }
void memalign(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).allocate(getRegU32(ctx,5),getRegU32(ctx,4))); }
void free(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ auto &allocator=heap(ram,ctx,runtime); release(allocator,getRegU32(ctx,4)); }
void malloc_r(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).allocate(getRegU32(ctx,5))); }
void calloc_r(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).calloc(getRegU32(ctx,5),getRegU32(ctx,6))); }
void realloc_r(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).reallocate(getRegU32(ctx,5),getRegU32(ctx,6))); }
void memalign_r(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ setReturnU32(ctx,heap(ram,ctx,runtime).allocate(getRegU32(ctx,6),getRegU32(ctx,5))); }
void free_r(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ auto &allocator=heap(ram,ctx,runtime); release(allocator,getRegU32(ctx,5)); }
}
