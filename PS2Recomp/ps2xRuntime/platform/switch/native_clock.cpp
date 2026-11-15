#include "native_clock.hpp"
#include <limits>
#include <stdexcept>
#ifdef __SWITCH__
#include <switch/arm/counter.h>
#else
#include <chrono>
#endif
namespace celosia::native {
namespace {
uint64_t readSystem(void *)
{
#ifdef __SWITCH__
    return armGetSystemTick();
#else
    const auto value=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if(value<0) throw std::runtime_error("Native monotonic clock has negative epoch");
    return static_cast<uint64_t>(value);
#endif
}
ClockSource systemSource()
{
#ifdef __SWITCH__
    return {readSystem,nullptr,armGetSystemTickFreq()};
#else
    return {readSystem,nullptr,1000000000};
#endif
}
}
uint64_t scaleClockTicks(uint64_t elapsed,uint64_t frequency,uint64_t logicalHz)
{
    if(!frequency || !logicalHz) throw std::invalid_argument("Native clock frequency is zero");
    const auto value=static_cast<unsigned __int128>(elapsed)*logicalHz/frequency;
    if(value>std::numeric_limits<uint64_t>::max()) throw std::overflow_error("Native clock duration overflow");
    return static_cast<uint64_t>(value);
}
SystemClock::SystemClock() : SystemClock(systemSource()) {}
SystemClock::SystemClock(uint64_t logicalHz) : SystemClock(systemSource(),logicalHz) {}
SystemClock::SystemClock(ClockSource clock,uint64_t logicalHz) : source(clock),logicalFrequency(logicalHz)
{
    if(!source.read || !source.frequency || !logicalFrequency) throw std::invalid_argument("Invalid native monotonic clock source");
}
bool SystemClock::initialize()
{
    std::lock_guard lock(mutex);
    if(ready) return false;
    accumulated=0; startedAt=0; lastObserved=0; observed=false;
    ready=true; active=false;
    return true;
}
uint64_t SystemClock::sample()
{
    const auto tick=source.read(source.user);
    if(observed && tick<lastObserved) throw std::runtime_error("Native monotonic clock moved backwards");
    lastObserved=tick; observed=true;
    return tick;
}
uint64_t SystemClock::elapsedAt(uint64_t tick) const
{
    const auto delta=tick-startedAt;
    if(delta>std::numeric_limits<uint64_t>::max()-accumulated)
        throw std::overflow_error("Native clock elapsed counter overflow");
    return accumulated+delta;
}
bool SystemClock::start()
{
    std::lock_guard lock(mutex);
    if(!ready) throw std::logic_error("Native clock is not initialized");
    if(active) return false;
    startedAt=sample(); active=true;
    return true;
}
bool SystemClock::stop()
{
    std::lock_guard lock(mutex);
    if(!ready) throw std::logic_error("Native clock is not initialized");
    if(!active) return false;
    const auto elapsed=elapsedAt(sample());
    // Reject a duration that cannot be returned before changing running state.
    (void)scaleClockTicks(elapsed,source.frequency,logicalFrequency);
    accumulated=elapsed; active=false;
    return true;
}
bool SystemClock::end()
{
    std::lock_guard lock(mutex);
    if(!ready) return false;
    ready=false; active=false; accumulated=0;
    return true;
}
uint64_t SystemClock::now()
{
    std::lock_guard lock(mutex);
    if(!ready) throw std::logic_error("Native clock is not initialized");
    return scaleClockTicks(active ? elapsedAt(sample()) : accumulated,source.frequency,logicalFrequency);
}
bool SystemClock::initialized() const { std::lock_guard lock(mutex); return ready; }
bool SystemClock::running() const { std::lock_guard lock(mutex); return active; }
}
