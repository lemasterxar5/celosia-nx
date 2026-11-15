#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
namespace celosia::native {
// Real OS workers, not a guest CPU scheduler. Callback execution and data
// consistency belong to the caller. Callbacks must respond to stopRequested().
// Before destroying callback-owned data: requestShutdown(), release execution
// locks, then require joinAll()==true from the constructing owner thread.
class NativeThreads final {
public:
    static constexpr int32_t Error=-1,Unknown=-408,Busy=-409,Deleted=-425;
    static constexpr unsigned MaxLive=256;
    using Entry=std::function<void(uint32_t id,uint32_t argument)>;
    struct Status {
        bool started,running,sleeping,finished,failed;
        uint32_t wakePermits;
        int priority;
        bool retired;
    };
    explicit NativeThreads(unsigned maxLive=MaxLive);
    ~NativeThreads();
    NativeThreads(const NativeThreads &)=delete;
    NativeThreads &operator=(const NativeThreads &)=delete;
    int32_t registerMain();
    int32_t create(Entry entry,int priority=0x2C,size_t stackBytes=0x40000);
    int32_t start(int32_t id,uint32_t argument);
    int32_t erase(int32_t id);
    int32_t restart(int32_t id,uint32_t argument);
    int32_t retireCurrent();
    bool reapRetired();
    int32_t sleepCurrent();
    int32_t wake(int32_t id);
    int32_t setPriority(int32_t id,int priority);
    int32_t currentId() const;
    std::optional<Status> status(int32_t id) const;
    bool stopRequested() const noexcept;
    void requestShutdown() noexcept;
    bool joinAll();
    bool shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
