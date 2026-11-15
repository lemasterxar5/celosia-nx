#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>
namespace native_renderer::detail::deko {
// Index allocator for one shared image descriptor pool. Binding a descriptor set in
// deko3d rewrites the GPU's texture header pool base (SetTexHeaderPool), so the
// renderer binds one large pool once per frame and selects textures by slot in the
// texture handle instead of switching pools per draw. Slots are released only when
// their Texture dies, which happens after every frame that referenced it retired.
class DescriptorSlots {
    uint32_t capacity, next=0;
    std::vector<uint32_t> free;
    uint32_t live=0;
public:
    static constexpr uint32_t None=UINT32_MAX;
    explicit DescriptorSlots(uint32_t slots):capacity(slots){
        if(!slots || slots>65536)throw std::invalid_argument("deko3d: invalid descriptor slot count");
    }
    uint32_t size()const{return capacity;}
    uint32_t inUse()const{return live;}
    // Lowest free index; None when the pool is exhausted (caller falls back to a private set).
    uint32_t acquire(){
        uint32_t slot;
        if(!free.empty()){slot=free.back();free.pop_back();}
        else if(next<capacity)slot=next++;
        else return None;
        ++live;return slot;
    }
    void release(uint32_t slot){
        if(slot==None)return;
        if(slot>=next || !live)throw std::logic_error("deko3d: descriptor slot release mismatch");
        for(const auto pending:free)if(pending==slot)throw std::logic_error("deko3d: descriptor slot double release");
        free.push_back(slot);--live;
    }
};
}
