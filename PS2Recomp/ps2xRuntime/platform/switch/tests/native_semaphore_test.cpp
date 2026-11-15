#include "native_semaphore.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <vector>

using celosia::native::SemaphoreSet;
using namespace std::chrono_literals;
static unsigned checks=0;
static void require(bool ok,const char *why)
{
    ++checks;
    if (!ok) { std::fprintf(stderr,"native semaphore failure: %s\n",why); std::exit(2); }
}
static void queued(SemaphoreSet &set,int32_t id,int32_t count)
{
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto status=set.status(id);
        if (status && status->waiters==count) return;
        std::this_thread::sleep_for(1ms);
    }
    require(false,"waiter failed to queue before deadline");
}
static int32_t completed(std::future<int32_t> &future)
{
    require(future.wait_for(2s)==std::future_status::ready,"waiter failed to complete before deadline");
    return future.get();
}
int main()
{
    SemaphoreSet set;
    require(set.create(-1,1,0,0)==SemaphoreSet::Error,"negative initial");
    require(set.create(0,0,0,0)==SemaphoreSet::Error,"zero maximum");
    require(set.create(2,1,0,0)==SemaphoreSet::Error,"initial exceeds maximum");
    require(set.create(0,1,1,0)==SemaphoreSet::Error,"unsupported scheduling attributes");
    const auto id=set.create(1,2,0,0x138250);
    require(id>0,"create native semaphore");
    const auto initial=set.status(id);
    require(initial && initial->count==1 && initial->max==2 && initial->initial==1 &&
            initial->waiters==0 && initial->attr==0 && initial->option==0x138250,"complete status metadata");
    require(set.poll(id)==id && set.poll(id)==SemaphoreSet::Empty,"poll consumes count and reports empty");
    require(set.signal(id)==id && set.signal(id)==id,"signal counts");
    require(set.signal(id)==SemaphoreSet::Overflow,"maximum count overflow");
    require(set.wait(id)==id && set.wait(id)==id,"immediate waits consume counts");
    auto waiter=std::async(std::launch::async,[&] { return set.wait(id); });
    queued(set,id,1);
    require(waiter.wait_for(20ms)==std::future_status::timeout,"wait really blocks native thread");
    require(set.signal(id)==id && completed(waiter)==id,"signal wakes waiter");
    require(set.status(id)->count==0,"handoff does not duplicate token");
    auto removed=std::async(std::launch::async,[&] { return set.wait(id); });
    queued(set,id,1);
    require(set.erase(id)==id && completed(removed)==SemaphoreSet::Deleted,"erase wakes deleted wait");
    require(!set.status(id),"deleted status absent");
    require(set.poll(id)==SemaphoreSet::Unknown && set.wait(id)==SemaphoreSet::Unknown &&
            set.signal(id)==SemaphoreSet::Unknown && set.erase(id)==SemaphoreSet::Unknown,"unknown handle errors");
    const auto newer=set.create(0,1,0,0);
    require(newer>id && set.signal(id)==SemaphoreSet::Unknown,"deleted handles never reused");
    std::vector<std::future<int32_t>> fifo;
    for(int32_t i=0;i<3;++i) {
        fifo.push_back(std::async(std::launch::async,[&] { return set.wait(newer); }));
        queued(set,newer,i+1);
    }
    for(size_t i=0;i<fifo.size();++i) {
        require(set.signal(newer)==newer,"FIFO signal");
        require(completed(fifo[i])==newer,"FIFO waiter completed");
        if(i+1<fifo.size()) require(fifo[i+1].wait_for(10ms)==std::future_status::timeout,"no spurious permit");
    }
    const auto large=set.create(INT32_MAX,INT32_MAX,0,0);
    require(large>0 && set.signal(large)==SemaphoreSet::Overflow,"large count is bounded metadata only");
    require(set.poll(large)==large && set.signal(large)==large,"large count cannot overflow");
    SemaphoreSet capacity;
    for(unsigned i=0;i<SemaphoreSet::MaxLive;++i) require(capacity.create(0,1,0,0)>0,"bounded handle capacity");
    require(capacity.create(0,1,0,0)==SemaphoreSet::Error,"resource capacity enforced");
    require(capacity.erase(1)==1 && capacity.create(0,1,0,0)>static_cast<int32_t>(SemaphoreSet::MaxLive),
            "capacity reuse preserves stale handle safety");
    const auto gate=set.create(1,1,0,0);
    int counter=0;
    std::atomic<bool> failed{false};
    std::vector<std::future<void>> workers;
    for(unsigned n=0;n<4;++n) workers.push_back(std::async(std::launch::async,[&] {
        for(unsigned i=0;i<500;++i) {
            if(set.wait(gate)!=gate) { failed=true; return; }
            ++counter;
            if(set.signal(gate)!=gate) { failed=true; return; }
        }
    }));
    for(auto &worker:workers) {
        require(worker.wait_for(5s)==std::future_status::ready,"concurrent workers bounded completion");
        worker.get();
    }
    require(!failed && counter==2000 && set.status(gate)->count==1,"mutual exclusion across native threads");
    const auto shutdownId=set.create(0,1,0,0);
    auto shutdownWait=std::async(std::launch::async,[&] { return set.wait(shutdownId); });
    queued(set,shutdownId,1); set.shutdown();
    require(completed(shutdownWait)==SemaphoreSet::Deleted,"shutdown wakes native waiters");
    require(set.create(0,1,0,0)==SemaphoreSet::Error && !set.status(gate),"shutdown rejects new resources");
    auto owner=std::make_unique<SemaphoreSet>();
    const auto endingId=owner->create(0,1,0,0);
    auto *raw=owner.get();
    auto ending=std::async(std::launch::async,[=] { return raw->wait(endingId); });
    queued(*owner,endingId,1); owner->shutdown();
    require(completed(ending)==SemaphoreSet::Deleted,"owner shutdown releases wait before native thread join");
    owner.reset();
    std::printf("native semaphores: %u checks passed\n",checks);
}
