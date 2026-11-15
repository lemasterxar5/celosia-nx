#include "native_semaphore.hpp"
#include <algorithm>
#include <deque>
#include <mutex>
#include <new>
#include <system_error>
#include <unordered_map>
#if defined(__SWITCH__)
extern "C" {
#include <switch/kernel/condvar.h>
#include <switch/kernel/mutex.h>
}
#else
#include <condition_variable>
#endif

namespace celosia::native {
namespace {
#if defined(__SWITCH__)
struct NativeMutex {
    Mutex value;
    NativeMutex() { mutexInit(&value); }
    void lock() { mutexLock(&value); }
    void unlock() { mutexUnlock(&value); }
};
struct NativeCondition {
    CondVar value;
    NativeCondition() { condvarInit(&value); }
    bool sleep(std::unique_lock<NativeMutex> &lock)
    { return condvarWait(&value,&lock.mutex()->value)==0; }
    void wakeAll() noexcept { (void)condvarWakeAll(&value); }
};
#else
using NativeMutex=std::mutex;
struct NativeCondition {
    std::condition_variable value;
    bool sleep(std::unique_lock<NativeMutex> &lock)
    {
        try { value.wait(lock); return true; }
        catch(const std::system_error &) { return false; }
    }
    void wakeAll() noexcept { value.notify_all(); }
};
#endif
struct Waiter { bool granted=false; };
struct State {
    NativeMutex mutex;
    NativeCondition changed;
    int32_t count,max,initial;
    uint32_t attr,option;
    bool closed=false;
    std::deque<Waiter *> waiters;
    State(int32_t start,int32_t maximum,uint32_t attributes,uint32_t opaque)
        : count(start),max(maximum),initial(start),attr(attributes),option(opaque) {}
    void close() noexcept
    {
        std::lock_guard lock(mutex);
        closed=true;
        changed.wakeAll();
    }
};
struct Registration {
    State &state;
    Waiter *waiter;
    ~Registration() { std::erase(state.waiters,waiter); }
};
}
struct SemaphoreSet::Impl {
    mutable NativeMutex mutex;
    std::unordered_map<int32_t,std::shared_ptr<State>> states;
    uint64_t next=1;
    bool closed=false;
    std::shared_ptr<State> find(int32_t id) const
    {
        std::lock_guard lock(mutex);
        const auto found=states.find(id);
        return found==states.end() ? nullptr : found->second;
    }
    void shutdown() noexcept
    {
        std::lock_guard lock(mutex);
        closed=true;
        for(const auto &[id,state]:states) { (void)id; state->close(); }
        states.clear();
    }
};
SemaphoreSet::SemaphoreSet() : impl(std::make_shared<Impl>()) {}
SemaphoreSet::~SemaphoreSet() { shutdown(); }
void SemaphoreSet::shutdown() noexcept { impl->shutdown(); }
int32_t SemaphoreSet::create(int32_t initial,int32_t max,uint32_t attr,uint32_t option)
{
    if(initial<0 || max<=0 || initial>max || attr!=0) return Error;
    std::lock_guard lock(impl->mutex);
    if(impl->closed || impl->states.size()>=MaxLive || impl->next>INT32_MAX) return Error;
    try {
        auto state=std::make_shared<State>(initial,max,attr,option);
        const auto id=static_cast<int32_t>(impl->next);
        impl->states.emplace(id,std::move(state));
        ++impl->next;
        return id;
    } catch(const std::bad_alloc &) { return Error; }
}
int32_t SemaphoreSet::erase(int32_t id)
{
    std::lock_guard lock(impl->mutex);
    const auto found=impl->states.find(id);
    if(found==impl->states.end()) return Unknown;
    found->second->close();
    impl->states.erase(found);
    return id;
}
int32_t SemaphoreSet::signal(int32_t id)
{
    const auto state=impl->find(id);
    if(!state) return Unknown;
    std::lock_guard lock(state->mutex);
    if(state->closed) return Unknown;
    if(!state->waiters.empty()) {
        state->waiters.front()->granted=true;
        state->waiters.pop_front();
        state->changed.wakeAll();
    } else {
        if(state->count==state->max) return Overflow;
        ++state->count;
    }
    return id;
}
int32_t SemaphoreSet::poll(int32_t id)
{
    const auto state=impl->find(id);
    if(!state) return Unknown;
    std::lock_guard lock(state->mutex);
    if(state->closed) return Unknown;
    if(!state->count) return Empty;
    --state->count;
    return id;
}
int32_t SemaphoreSet::wait(int32_t id)
{
    const auto state=impl->find(id);
    if(!state) return Unknown;
    std::unique_lock lock(state->mutex);
    if(state->closed) return Unknown;
    if(state->count) { --state->count; return id; }
    if(state->waiters.size()>=static_cast<size_t>(INT32_MAX)) return Error;
    Waiter waiter;
    try { state->waiters.push_back(&waiter); }
    catch(const std::bad_alloc &) { return Error; }
    Registration registration{*state,&waiter};
    while(!waiter.granted && !state->closed) {
        if(!state->changed.sleep(lock)) {
            if(waiter.granted) break;
            return Error;
        }
    }
    return waiter.granted ? id : Deleted;
}
std::optional<SemaphoreSet::Status> SemaphoreSet::status(int32_t id) const
{
    const auto state=impl->find(id);
    if(!state) return std::nullopt;
    std::lock_guard lock(state->mutex);
    if(state->closed) return std::nullopt;
    return Status{state->count,state->max,state->initial,static_cast<int32_t>(state->waiters.size()),
                  state->attr,state->option};
}
}
