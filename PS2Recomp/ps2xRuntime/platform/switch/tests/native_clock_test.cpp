#include "native_clock.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
template<class E,class F> void rejects(F action,const char *why)
{ bool caught=false; try { action(); } catch(const E &) { caught=true; } require(caught,why); }
uint64_t readFake(void *p) { return *static_cast<uint64_t *>(p); }
}
int main()
{
    constexpr auto Max=std::numeric_limits<uint64_t>::max();
    require(scaleClockTicks(0,1)==0,"zero duration");
    require(scaleClockTicks(19200000,19200000)==147456000,"Switch second conversion");
    require(scaleClockTicks(1000000000,1000000000)==147456000,"host second conversion");
    require(scaleClockTicks(625,19200000)==4800,"exact rational conversion");
    require(scaleClockTicks(1,1000000000)==0,"fraction floors");
    require(scaleClockTicks(Max,Max)==SystemClock::LogicalHz,"wide multiplication");
    require(scaleClockTicks(Max,SystemClock::LogicalHz)==Max,"maximum result accepted");
    rejects<std::invalid_argument>([]{ (void)scaleClockTicks(1,0); },"zero frequency rejected");
    rejects<std::overflow_error>([&]{ (void)scaleClockTicks(Max,1); },"result overflow rejected");
    rejects<std::invalid_argument>([]{ SystemClock c({nullptr,nullptr,1}); },"null source rejected");
    uint64_t tick=100;
    rejects<std::invalid_argument>([&]{ SystemClock c({readFake,&tick,0}); },"invalid source frequency");
    SystemClock c({readFake,&tick,19200000});
    require(!c.initialized() && !c.running(),"initially uninitialized");
    rejects<std::logic_error>([&]{ (void)c.now(); },"uninitialized read");
    rejects<std::logic_error>([&]{ (void)c.start(); },"uninitialized start");
    rejects<std::logic_error>([&]{ (void)c.stop(); },"uninitialized stop");
    require(!c.end(),"inactive end has no effect");
    require(c.initialize() && c.initialized() && !c.running() && c.now()==0,"initialize stopped at zero");
    require(!c.initialize(),"duplicate initialize rejected");
    require(!c.stop(),"already stopped");
    require(c.start() && c.running(),"starts");
    tick+=19200000;
    require(c.now()==SystemClock::LogicalHz,"one second native elapsed");
    require(!c.start() && c.now()==SystemClock::LogicalHz,"duplicate start preserves elapsed");
    require(c.stop() && !c.running(),"stops");
    tick+=19200000;
    require(c.now()==SystemClock::LogicalHz,"paused wall time excluded");
    require(c.start(),"restarts"); tick+=19200000;
    require(c.now()==2*SystemClock::LogicalHz,"resume preserves prior elapsed");
    require(c.end() && !c.initialized() && !c.running(),"end destroys scalar timer state");
    require(c.initialize() && c.now()==0,"reinitialize resets elapsed");
    require(c.start(),"start after reinitialize"); tick-=1;
    rejects<std::runtime_error>([&]{ (void)c.now(); },"backward clock rejected");
    rejects<std::runtime_error>([&]{ (void)c.stop(); },"backward stop rejected");
    require(c.running(),"failed stop preserves state"); tick+=2;
    require(c.stop(),"recovers after valid counter");
    uint64_t fine=0; SystemClock fractional({readFake,&fine,1000000000});
    fractional.initialize();
    for(unsigned i=0;i<100;++i) { fractional.start(); ++fine; fractional.stop(); fine+=99; }
    require(fractional.now()==14,"fraction preserved across small intervals");
    uint64_t wide=0; SystemClock overflow({readFake,&wide,SystemClock::LogicalHz});
    overflow.initialize(); overflow.start(); wide=Max;
    require(overflow.stop() && overflow.now()==Max,"maximum accumulation accepted");
    overflow.start();
    // A monotonic 64-bit source cannot advance after UINT64_MAX: reset is rejected.
    wide=0;
    rejects<std::runtime_error>([&]{ (void)overflow.now(); },"source wrap fails explicitly");
    SystemClock real;
    require(real.initialize() && real.start(),"actual OS clock starts");
    const auto before=real.now(),after=real.now();
    require(after>=before,"actual OS clock monotonic");
    require(real.stop() && real.end(),"actual OS clock closes");
    uint64_t kernelTicks=0;
    SystemClock kernel({readFake,&kernelTicks,19200000},150000000);
    require(kernel.initialize() && kernel.start(),"native kernel duration starts in documented scalar units");
    kernelTicks=19200000;
    require(kernel.now()==150000000,"kernel second uses its actual API conversion denominator");
    require(kernel.stop() && kernel.end(),"kernel scalar duration closes");
    rejects<std::invalid_argument>([&]{SystemClock invalid({readFake,&kernelTicks,19200000},0);},"zero logical frequency rejected");
    std::printf("native clock: %u checks passed\n",checks);
}
