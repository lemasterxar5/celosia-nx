#pragma once
#include <cstdint>
#include <memory>
#include <optional>

namespace celosia::native {
// Native synchronization resources; no CPU clock or guest thread scheduler.
// Call shutdown() before joining waiting native threads. This owner must outlive
// all calls; starting operations concurrently with destruction is unsupported.
class SemaphoreSet final {
public:
    static constexpr int32_t Error=-1, Unknown=-408, Empty=-419, Overflow=-420, Deleted=-425;
    static constexpr unsigned MaxLive=256;
    struct Status {
        int32_t count,max,initial,waiters;
        uint32_t attr,option;
    };
    SemaphoreSet();
    ~SemaphoreSet();
    SemaphoreSet(const SemaphoreSet &)=delete;
    SemaphoreSet &operator=(const SemaphoreSet &)=delete;
    int32_t create(int32_t initial,int32_t max,uint32_t attr,uint32_t option);
    int32_t erase(int32_t id);
    int32_t signal(int32_t id);
    int32_t poll(int32_t id);
    int32_t wait(int32_t id);
    std::optional<Status> status(int32_t id) const;
    void shutdown() noexcept;
private:
    struct Impl;
    const std::shared_ptr<Impl> impl;
};
}
