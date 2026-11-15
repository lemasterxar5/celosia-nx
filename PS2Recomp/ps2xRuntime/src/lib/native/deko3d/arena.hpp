#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace native_renderer::detail::deko {
// Reset only after the owning frame fence completes; existing pages stay live.
class ArenaCursor {
    static constexpr uint32_t Page=1024u*1024u,Limit=128u*1024u*1024u;
    std::vector<uint32_t> capacities;
    size_t page=0;
    uint32_t offset=0,allocated=0;
public:
    struct Span {size_t page;uint32_t offset,capacity;};
    void reset()noexcept{page=0;offset=0;}
    Span reserve(size_t bytes){
        if(!bytes || bytes>Limit)throw std::invalid_argument("Invalid transient vertex range");
        const auto aligned=static_cast<uint32_t>((bytes+15u)&~size_t(15u));
        while(page<capacities.size() && aligned>capacities[page]-offset){++page;offset=0;}
        if(page==capacities.size()){
            const auto capacity=std::max(Page,aligned);
            if(capacity>Limit-allocated)throw std::length_error("Transient frame exceeds 128 MiB");
            capacities.push_back(capacity);allocated+=capacity;
        }
        const Span result{page,offset,capacities[page]};offset+=aligned;return result;
    }
};
}
