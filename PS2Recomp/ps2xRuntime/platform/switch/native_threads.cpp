#include "native_threads.hpp"
#include <atomic>
#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>
#ifdef __SWITCH__
extern "C" {
#include <switch.h>
}
#else
#include <condition_variable>
#include <thread>
#endif
namespace celosia::native {
namespace {
thread_local const void *currentManager=nullptr;
thread_local int32_t currentTask=NativeThreads::Unknown;
#ifdef __SWITCH__
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
    { return R_SUCCEEDED(condvarWait(&value,&lock.mutex()->value)); }
    void wakeAll() noexcept { (void)condvarWakeAll(&value); }
};
using NativeIdentity=uint64_t;
NativeIdentity nativeIdentity()
{
    u64 id=0;
    if(R_FAILED(svcGetThreadId(&id,CUR_THREAD_HANDLE))) throw std::runtime_error("Cannot identify native thread");
    return id;
}
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
using NativeIdentity=std::thread::id;
NativeIdentity nativeIdentity() { return std::this_thread::get_id(); }
#endif
}
struct NativeThreads::Impl {
    struct Task {
        Impl *owner;
        Entry entry;
        int32_t id;
        int priority;
        size_t stackBytes=0x40000;
        uint32_t argument=0,permits=0;
        bool main=false,started=false,running=false,sleeping=false,finished=false,failed=false;
        bool cancel=false,joining=false,retired=false;
#ifdef __SWITCH__
        Thread thread{};
        Handle mainHandle=INVALID_HANDLE;
        bool created=false,launched=false;
#else
        std::thread thread;
#endif
        Task(Impl *o,Entry fn,int32_t own,int prio):owner(o),entry(std::move(fn)),id(own),priority(prio) {}
    };
    mutable NativeMutex mutex;
    NativeCondition changed;
    std::unordered_map<int32_t,std::unique_ptr<Task>> tasks;
    const NativeIdentity owner=nativeIdentity();
    const unsigned capacity;
    uint64_t next=1;
    int32_t mainId=Unknown;
    std::atomic<bool> stopping{false};
    explicit Impl(unsigned cap):capacity(cap) {}
    Task *find(int32_t id) const
    { const auto i=tasks.find(id); return i==tasks.end() ? nullptr : i->second.get(); }
    bool isOwner() const { return nativeIdentity()==owner; }
    static void run(void *pointer) noexcept
    {
        auto &task=*static_cast<Task *>(pointer);
        auto &state=*task.owner;
        currentManager=&state; currentTask=task.id;
        {
            std::unique_lock lock(state.mutex);
            while(!task.started && !task.cancel && !state.stopping.load()) {
                if(!state.changed.sleep(lock)) { task.failed=true; task.cancel=true; }
            }
            if(!task.cancel && !state.stopping.load()) task.running=true;
        }
        if(task.running) {
            try { task.entry(static_cast<uint32_t>(task.id),task.argument); }
            catch(...) { std::lock_guard lock(state.mutex); task.failed=true; }
        }
        {
            std::lock_guard lock(state.mutex);
            task.running=false; task.sleeping=false; task.finished=true;
            state.changed.wakeAll();
        }
        currentTask=Unknown; currentManager=nullptr;
    }
    bool launch(Task &task,size_t stackBytes)
    {
#ifdef __SWITCH__
        task.thread={};
        auto rc=threadCreate(&task.thread,run,&task,nullptr,stackBytes,task.priority,-2);
        if(R_FAILED(rc)) { std::fprintf(stderr,"Native threadCreate failed: 0x%x\n",rc); return false; }
        task.created=true;
        rc=threadStart(&task.thread);
        if(R_SUCCEEDED(rc)) { task.launched=true; return true; }
        std::fprintf(stderr,"Native threadStart failed: 0x%x\n",rc);
        rc=threadClose(&task.thread);
        if(R_SUCCEEDED(rc)) task.created=false;
        else std::fprintf(stderr,"Native threadClose after start failure: 0x%x; ownership retained\n",rc);
        return false;
#else
        (void)stackBytes;
        try { task.thread=std::thread([&task]{ run(&task); }); return true; }
        catch(const std::system_error &) { return false; }
        catch(const std::bad_alloc &) { return false; }
#endif
    }
    bool join(Task &task)
    {
#ifdef __SWITCH__
        if(task.launched) {
            if(R_FAILED(threadWaitForExit(&task.thread))) return false;
            task.launched=false;
        }
        if(task.created) {
            if(R_FAILED(threadClose(&task.thread))) return false;
            task.created=false;
        }
#else
        if(task.thread.joinable()) {
            try { task.thread.join(); } catch(const std::system_error &) { return false; }
        }
#endif
        return true;
    }
    bool hasResources(const Task &task) const noexcept
    {
#ifdef __SWITCH__
        return task.created || task.launched;
#else
        return task.thread.joinable();
#endif
    }
};
NativeThreads::NativeThreads(unsigned maxLive)
{
    if(!maxLive || maxLive>MaxLive) throw std::invalid_argument("Invalid native thread capacity");
    impl=std::make_unique<Impl>(maxLive);
}
NativeThreads::~NativeThreads()
{
    requestShutdown();
    if(!joinAll()) {
        // No forced termination, detaching or freeing callback storage while a
        // worker may still use it. The caller must verify joinAll before it
        // destroys callback-owned data; this path only retains worker resources.
        std::fprintf(stderr,"Native thread join failed; retaining worker resources\n");
        (void)impl.release();
    }
}
int32_t NativeThreads::registerMain()
{
    if(!impl->isOwner()) return Error;
    std::lock_guard lock(impl->mutex);
    if(impl->stopping.load()) return Error;
    if(impl->mainId>0) return impl->mainId;
    if(currentManager || impl->tasks.size()>=impl->capacity || impl->next>INT32_MAX) return Error;
    try {
        const auto id=static_cast<int32_t>(impl->next++);
        auto task=std::make_unique<Impl::Task>(impl.get(),Entry{},id,0x2C);
        task->main=true; task->started=true; task->running=true;
#ifdef __SWITCH__
          task->mainHandle=threadGetCurHandle();
#endif
        impl->tasks.emplace(id,std::move(task));
        impl->mainId=id; currentManager=impl.get(); currentTask=id;
        return id;
    } catch(const std::bad_alloc &) { return Error; }
}
int32_t NativeThreads::create(Entry entry,int priority,size_t stackBytes)
{
    if(!entry || priority<0 || priority>63 || stackBytes<0x4000 || stackBytes>0x1000000 || (stackBytes&0xFFF)) return Error;
    std::lock_guard lock(impl->mutex);
    if(impl->stopping.load() || impl->tasks.size()>=impl->capacity || impl->next>INT32_MAX) return Error;
    try {
        const auto id=static_cast<int32_t>(impl->next++);
        auto task=std::make_unique<Impl::Task>(impl.get(),std::move(entry),id,priority);
        auto *raw=task.get(); raw->stackBytes=stackBytes;
        impl->tasks.emplace(id,std::move(task));
        if(!impl->launch(*raw,stackBytes)) {
            if(!impl->hasResources(*raw)) impl->tasks.erase(id);
            else { raw->cancel=true; raw->failed=true; raw->finished=true; raw->retired=true; }
            return Error;
        }
        return id;
    } catch(const std::bad_alloc &) { return Error; }
}
int32_t NativeThreads::start(int32_t id,uint32_t argument)
{
    std::unique_lock lock(impl->mutex);
    auto *task=impl->find(id);
    if(!task || task->retired) return Unknown;
    if(impl->stopping.load()) return Deleted;
    if(task->finished && !task->joining) { lock.unlock(); return restart(id,argument); }
    if(task->started || task->cancel || task->joining) return Busy;
    task->argument=argument; task->started=true;
    impl->changed.wakeAll();
    return id;
}
int32_t NativeThreads::erase(int32_t id)
{
    Impl::Task *task;
    {
        std::lock_guard lock(impl->mutex);
        task=impl->find(id);
        if(!task) return Unknown;
        if(task->main || task->running || task->joining || (task->started && !task->finished)) return Busy;
        task->cancel=true; task->joining=true; impl->changed.wakeAll();
    }
    const bool joined=impl->join(*task);
    std::lock_guard lock(impl->mutex);
    if(!joined) { task->joining=false; impl->changed.wakeAll(); return Error; }
    impl->tasks.erase(id); impl->changed.wakeAll();
    return id;
}

int32_t NativeThreads::restart(int32_t id,uint32_t argument)
{
    Impl::Task *task;
    {
        std::lock_guard lock(impl->mutex);
        task=impl->find(id);
        if(!task || task->retired) return Unknown;
        if(impl->stopping.load()) return Deleted;
        if(!task->finished || task->joining || task->main) return Busy;
        task->joining=true;
    }
    const bool joined=impl->join(*task);
    std::lock_guard lock(impl->mutex);
    task->joining=false; impl->changed.wakeAll();
    if(!joined) return Error;
    if(impl->stopping.load()) return Deleted;
    task->argument=argument; task->permits=0; task->started=true;
    task->running=false; task->sleeping=false; task->finished=false; task->failed=false;
    task->cancel=false;
    if(!impl->launch(*task,task->stackBytes)) { task->finished=true; task->failed=true; return Error; }
    return id;
}
int32_t NativeThreads::retireCurrent()
{
    const auto id=currentId();
    std::lock_guard lock(impl->mutex);
    auto *task=impl->find(id);
    if(!task) return Unknown;
    if(task->main) return Busy;
    task->retired=true;
    return id;
}
bool NativeThreads::reapRetired()
{
    std::array<Impl::Task *,MaxLive> pending{};
    unsigned count=0;
    {
        std::lock_guard lock(impl->mutex);
        for(auto &[id,t]:impl->tasks) {
            if(t->retired && t->finished && !t->joining && id!=currentId()) {
                t->joining=true; pending[count++]=t.get();
            }
        }
    }
    bool all=true;
    for(unsigned i=0;i<count;++i) {
        auto *task=pending[i];
        const bool joined=impl->join(*task);
        std::lock_guard lock(impl->mutex);
        if(joined) impl->tasks.erase(task->id);
        else { task->joining=false; all=false; }
        impl->changed.wakeAll();
    }
    return all;
}
int32_t NativeThreads::currentId() const
{ return currentManager==impl.get() ? currentTask : Unknown; }
int32_t NativeThreads::sleepCurrent()
{
    const auto id=currentId();
    std::unique_lock lock(impl->mutex);
    auto *task=impl->find(id);
    if(!task) return Unknown;
    task->sleeping=true;
    while(!task->permits && !impl->stopping.load()) {
        if(!impl->changed.sleep(lock)) { task->sleeping=false; return Error; }
    }
    task->sleeping=false;
    if(impl->stopping.load()) return Deleted;
    --task->permits;
    return id;
}
int32_t NativeThreads::wake(int32_t id)
{
    std::lock_guard lock(impl->mutex);
    auto *task=impl->find(id);
    if(!task) return Unknown;
    if(impl->stopping.load()) return Deleted;
    if(task->finished || task->cancel || task->joining) return Busy;
    if(task->permits==std::numeric_limits<uint32_t>::max()) return Error;
    ++task->permits; impl->changed.wakeAll();
    return id;
}
int32_t NativeThreads::setPriority(int32_t id,int priority)
{
    std::lock_guard lock(impl->mutex);
    if(priority<0 || priority>63) return Error;
    auto *task=impl->find(id);
    if(!task || task->retired) return Unknown;
    if(impl->stopping.load()) return Deleted;
    // join/close runs outside mutex after publishing joining. Never read a
    // handle while another owner may close or replace the underlying OS task.
    if(task->joining || task->finished || task->cancel) return Busy;
#ifdef __SWITCH__
    const auto handle=task->main ? task->mainHandle : task->thread.handle;
    if(handle==INVALID_HANDLE || (handle==CUR_THREAD_HANDLE && id!=currentId())) return Error;
    const auto result=svcSetThreadPriority(handle,priority);
    if(R_FAILED(result)) { std::fprintf(stderr,"Native thread priority failed: 0x%x\n",result); return Error; }
    task->priority=priority;
    return id;
#else
    // Host tests do not simulate the Switch process's scheduling privileges.
    return Error;
#endif
}
std::optional<NativeThreads::Status> NativeThreads::status(int32_t id) const
{
    std::lock_guard lock(impl->mutex);
    const auto *t=impl->find(id);
    if(!t) return std::nullopt;
    return Status{t->started,t->running,t->sleeping,t->finished,t->failed,t->permits,t->priority,t->retired};
}
bool NativeThreads::stopRequested() const noexcept { return impl->stopping.load(std::memory_order_acquire); }
void NativeThreads::requestShutdown() noexcept
{
    impl->stopping.store(true,std::memory_order_release);
    std::lock_guard lock(impl->mutex);
    impl->changed.wakeAll();
}
bool NativeThreads::joinAll()
{
    if(!impl->isOwner()) return false;
    if(!impl->stopping.load()) return false;
    std::array<Impl::Task *,MaxLive> pending{};
    unsigned count=0;
    {
        std::unique_lock lock(impl->mutex);
        for(;;) {
            bool joining=false;
            for(const auto &[id,t]:impl->tasks) { (void)id; joining=joining || t->joining; }
            if(!joining) break;
            if(!impl->changed.sleep(lock)) return false;
        }
        for(auto &[id,t]:impl->tasks) {
            (void)id;
            if(!t->main) pending[count++]=t.get();
        }
        for(unsigned i=0;i<count;++i) pending[i]->joining=true;
    }
    bool all=true;
    for(unsigned i=0;i<count;++i) if(!impl->join(*pending[i])) all=false;
    std::lock_guard lock(impl->mutex);
    if(!all) { for(unsigned i=0;i<count;++i) pending[i]->joining=false; return false; }
    impl->tasks.clear(); impl->mainId=Unknown;
    if(currentManager==impl.get()) { currentManager=nullptr; currentTask=Unknown; }
    return true;
}
bool NativeThreads::shutdown() { requestShutdown(); return joinAll(); }
}


