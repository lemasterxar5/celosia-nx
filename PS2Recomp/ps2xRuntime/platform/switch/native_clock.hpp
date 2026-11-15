#pragma once
#include <cstdint>
#include <mutex>
namespace celosia::native {
struct ClockSource {
    uint64_t (*read)(void *);
    void *user;
    uint64_t frequency;
};
// Converts elapsed native counter units to the game's documented time unit.
// This is a scalar duration conversion; no hardware counter/register is modeled.
uint64_t scaleClockTicks(uint64_t elapsed,uint64_t frequency,uint64_t logicalHz=147456000);
class SystemClock final {
public:
    static constexpr uint64_t LogicalHz=147456000;
    SystemClock();
    explicit SystemClock(uint64_t logicalHz);
    explicit SystemClock(ClockSource source,uint64_t logicalHz=LogicalHz);
    bool initialize();
    bool start();
    bool stop();
    bool end();
    uint64_t now();
    bool initialized() const;
    bool running() const;
private:
    uint64_t sample();
    uint64_t elapsedAt(uint64_t tick) const;
    ClockSource source;
    const uint64_t logicalFrequency;
    mutable std::mutex mutex;
    uint64_t accumulated=0,startedAt=0,lastObserved=0;
    bool ready=false,active=false,observed=false;
};
}
