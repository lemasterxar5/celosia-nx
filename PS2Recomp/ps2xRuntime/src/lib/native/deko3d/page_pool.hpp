#pragma once
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
namespace native_renderer::detail::deko {
// Owner-thread-only allocator. A retained lease prevents reuse, including when
// its pool has gone away. Whole 4-KiB slots preserve existing GPU alignments.
template<class Resource,uint32_t Slots=1024> class PagePool {
    struct Page {
        std::shared_ptr<Resource> resource;
        std::bitset<Slots> used;
        uint32_t hint=0;
        uint32_t freeSlots=Slots; // v111: pages without room for a request are skipped unscanned
    };
public:
    static constexpr uint32_t SlotBytes=4096,PageBytes=Slots*SlotBytes;
    struct Lease {
        const std::shared_ptr<Resource> resource;
        const uint32_t offset;
        std::shared_ptr<Page> page;
        uint32_t first,count;
        Lease(std::shared_ptr<Page> p,uint32_t start,uint32_t length)
            :resource(p->resource),offset(start*SlotBytes),page(std::move(p)),first(start),count(length){}
        ~Lease(){for(uint32_t i=first;i<first+count;++i)page->used.reset(i);page->hint=std::min(page->hint,first);page->freeSlots+=count;}
        Lease(const Lease &)=delete;
        Lease &operator=(const Lease &)=delete;
    };
private:
    std::function<std::shared_ptr<Resource>()> create;
    const size_t limit;
    std::vector<std::shared_ptr<Page>> pages;
    static std::shared_ptr<Lease> reserve(const std::shared_ptr<Page> &page,uint32_t count){
        if(page->freeSlots<count)return {};
        for(uint32_t first=page->hint;first<=Slots-count;){
            uint32_t available=0;
            while(available<count && !page->used[first+available])++available;
            if(available==count){
                auto lease=std::make_shared<Lease>(page,first,count);
                for(uint32_t i=first;i<first+count;++i)page->used.set(i);
                page->freeSlots-=count;
                while(page->hint<Slots && page->used[page->hint])++page->hint;
                return lease;
            }
            first+=available+1;
        }
        return {};
    }
public:
    PagePool(std::function<std::shared_ptr<Resource>()> factory,size_t maxPages)
        :create(std::move(factory)),limit(maxPages){
        if(!create || !limit)throw std::invalid_argument("Invalid page pool");
    }
    std::shared_ptr<Lease> acquire(size_t bytes){
        if(!bytes || bytes>PageBytes)throw std::invalid_argument("Invalid pooled range");
        const auto count=static_cast<uint32_t>((bytes+SlotBytes-1)/SlotBytes);
        for(const auto &page:pages)if(auto lease=reserve(page,count))return lease;
        if(pages.size()>=limit)throw std::length_error("GPU page pool capacity exceeded");
        auto page=std::make_shared<Page>();page->resource=create();
        if(!page->resource)throw std::runtime_error("GPU page factory failed");
        pages.push_back(page);return reserve(page,count);
    }
};
}
