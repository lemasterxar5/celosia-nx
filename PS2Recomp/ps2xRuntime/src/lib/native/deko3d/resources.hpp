#pragma once
#include "policy.hpp"
#include "page_pool.hpp"
#include <deko3d.h>
#include <switch.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace native_renderer::detail::deko {
template<class T, void (*Destroy)(T)> struct Unique {
    T value{};
    Unique() = default;
    Unique(const Unique &) = delete;
    Unique &operator=(const Unique &) = delete;
    ~Unique() { reset(); }
    void reset(T next = {}) { if (value) Destroy(value); value = next; }
    operator T() const { return value; }
};
inline uint32_t alignSize(size_t n, uint32_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) ||
        n > std::numeric_limits<uint32_t>::max() - alignment)
        throw std::invalid_argument("deko3d: invalid allocation size/alignment");
    return (static_cast<uint32_t>(n) + alignment - 1) & ~(alignment - 1);
}
struct BufferBlock {
    static inline uint32_t created=0; // v109: dedicated memblocks made (slow-frame breakdown)
    Unique<DkMemBlock, dkMemBlockDestroy> memory;
    uint32_t bytes;
    std::shared_ptr<AllocationBudget> budget;
    BufferBlock(DkDevice device, size_t size, std::shared_ptr<AllocationBudget> allocationBudget, uint32_t flags =
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        : bytes(alignSize(size, DK_MEMBLOCK_ALIGNMENT)), budget(std::move(allocationBudget)) {
        if (!budget || !budget->tryReserve(bytes))
            throw std::runtime_error("deko3d: aggregate resource budget exceeded (512 MiB)");
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(&maker, device, bytes); maker.flags = flags;
        memory.reset(dkMemBlockCreate(&maker));++created;
        if (!memory.value) {
            budget->release(bytes);
            throw std::runtime_error("deko3d: memory allocation failed");
        }
    }
    ~BufferBlock() { memory.reset(); budget->release(bytes); }
    void *cpu() const { return dkMemBlockGetCpuAddr(memory); }
    DkGpuAddr gpu() const { return dkMemBlockGetGpuAddr(memory); }
};
using BufferPool=PagePool<BufferBlock>;
struct Buffer {
    std::shared_ptr<BufferBlock> block;
    std::shared_ptr<BufferPool::Lease> lease;
    uint32_t bytes,offset=0;
    static constexpr uint32_t DefaultFlags=DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached;
    Buffer(DkDevice device,size_t size,std::shared_ptr<AllocationBudget> budget,
           uint32_t flags=DefaultFlags,std::shared_ptr<BufferPool> pool={})
        :bytes(alignSize(size,DK_MEMBLOCK_ALIGNMENT)){
        if(pool && flags==DefaultFlags && bytes<=65536){
            lease=pool->acquire(bytes);block=lease->resource;offset=lease->offset;
        }else block=std::make_shared<BufferBlock>(device,bytes,std::move(budget),flags);
    }
    Buffer(DkDevice device,size_t size,std::shared_ptr<AllocationBudget> budget,const std::shared_ptr<BufferPool> &pool)
        :Buffer(device,size,std::move(budget),DefaultFlags,pool){}
    // v108: a range of a pooled page (textures and their staging; a dedicated memblock per
    // resource cost ~0.8 ms of the render thread each at scene changes).
    Buffer(std::shared_ptr<BufferPool::Lease> range,size_t size)
        :block(range->resource),lease(std::move(range)),bytes(alignSize(size,DK_MEMBLOCK_ALIGNMENT)){offset=lease->offset;}
    static std::shared_ptr<Buffer> pooled(const std::shared_ptr<BufferPool> &pool,size_t size){
        if(!pool || !size || size>BufferPool::PageBytes)return {};
        try{return std::make_shared<Buffer>(pool->acquire(size),size);}catch(const std::exception &){return {};}
    }
    DkMemBlock handle()const{return block->memory;}
    void *cpu()const{return static_cast<uint8_t *>(block->cpu())+offset;}
    DkGpuAddr gpu()const{return block->gpu()+offset;}
};
struct Image {
    std::shared_ptr<Buffer> storage;
    DkImage image{};
    Image(DkDevice device, uint32_t width, uint32_t height,
          DkImageFormat format, uint32_t flags, std::shared_ptr<AllocationBudget> budget,
          uint32_t levels = 1, const std::shared_ptr<BufferPool> &pool = {}, DkMsMode samples = DkMsMode_1x, bool zeroFill = false) {
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, device);
        maker.format = format; maker.flags = flags; maker.msMode = samples;
        maker.dimensions[0] = width; maker.dimensions[1] = height;
        maker.mipLevels = levels;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &maker);
        // Compressed render targets can require more than the memblock alignment.
        const uint32_t alignment=dkImageLayoutGetAlignment(&layout);
        // v108: sampled images from a pooled page when the layout's alignment fits a page slot.
        if(pool && alignment<=BufferPool::SlotBytes)storage=Buffer::pooled(pool,dkImageLayoutGetSize(&layout));
        if(!storage)storage = std::make_shared<Buffer>(device, dkImageLayoutGetSize(&layout), std::move(budget),
            DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image | (zeroFill ? DkMemBlockFlags_ZeroFillInit : 0u));
        if(alignment && storage->gpu()%alignment)
            throw std::runtime_error("deko3d: image storage misaligned for its layout");
        // Dedicated allocations start at zero, satisfying every image offset alignment.
        dkImageInitialize(&image, &layout, storage->handle(), storage->offset);
    }
    DkImageView view() const {
        DkImageView result; dkImageViewDefaults(&result, &image); return result;
    }
};
struct Shader {
    std::shared_ptr<Buffer> storage;
    DkShader shader{};
    Shader(DkDevice device, const char *path, DkStage stage, std::shared_ptr<AllocationBudget> budget) {
        using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
        File file(std::fopen(path, "rb"), &std::fclose);
        if (!file || std::fseek(file.get(), 0, SEEK_END))
            throw std::runtime_error(std::string("deko3d: cannot open shader ") + path);
        const long size = std::ftell(file.get());
        if (size < 32 || size > 4 * 1024 * 1024 || std::fseek(file.get(), 0, SEEK_SET))
            throw std::runtime_error("deko3d: invalid DKSH size");
        storage = std::make_shared<Buffer>(device,
            size_t(size) + DK_SHADER_CODE_UNUSABLE_SIZE, std::move(budget),
            DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
        if (std::fread(storage->cpu(), 1, size, file.get()) != size_t(size) ||
            std::memcmp(storage->cpu(), "DKSH", 4))
            throw std::runtime_error("deko3d: truncated/invalid DKSH");
        DkShaderMaker maker;
        dkShaderMakerDefaults(&maker, storage->handle(), storage->offset);
        dkShaderInitialize(&shader, &maker);
        if (!dkShaderIsValid(&shader) || dkShaderGetStage(&shader) != stage)
            throw std::runtime_error("deko3d: incompatible shader stage");
    }
};
}
