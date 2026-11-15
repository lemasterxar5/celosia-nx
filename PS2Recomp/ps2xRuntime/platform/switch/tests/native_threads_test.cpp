#include "native_threads.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
template<class F> void until(F predicate,const char *why)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) { if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error(why); std::this_thread::yield(); }
}
}
int main()
{
    bool invalidZero=false,invalidCap=false;
    try { NativeThreads zero(0); } catch(const std::invalid_argument &) { invalidZero=true; }
    try { NativeThreads large(NativeThreads::MaxLive+1); } catch(const std::invalid_argument &) { invalidCap=true; }
    require(invalidZero && invalidCap,"capacity validated");
    NativeThreads t(8);
    require(!t.joinAll(),"owner cannot join live registry before shutdown");
    require(t.currentId()==NativeThreads::Unknown,"unregistered native thread");
    require(t.sleepCurrent()==NativeThreads::Unknown,"unregistered cannot sleep");
    const auto main=t.registerMain();
    require(main>0 && t.currentId()==main,"main actual thread registered");
    require(t.registerMain()==main,"idempotent main registration");
    {
        NativeThreads second;
        require(second.registerMain()==NativeThreads::Error,"one actual thread cannot bind two registries");
    }
    require(t.currentId()==main,"foreign registry destruction preserves binding");
    require(t.status(main).has_value(),"main status");
    require(t.erase(main)==NativeThreads::Busy,"cannot erase executing main");
    require(t.wake(main)==main && t.sleepCurrent()==main,"wake before sleep retained");
    require(t.create({})==NativeThreads::Error,"null entry rejected");
    require(t.create([](uint32_t,uint32_t){},64)==NativeThreads::Error,"bad native priority rejected");
    require(t.create([](uint32_t,uint32_t){},0x2c,1)==NativeThreads::Error,"bad native stack rejected");
    require(t.create([](uint32_t,uint32_t){},-1)==NativeThreads::Error,"negative priority rejected");
    require(t.create([](uint32_t,uint32_t){},0x2c,0x1001000)==NativeThreads::Error,"oversize stack rejected");
    require(!t.status(-1),"unknown status absent");
    require(t.start(-1,0)==NativeThreads::Unknown && t.wake(-1)==NativeThreads::Unknown,"unknown operations");
    std::atomic<bool> invoked=false; std::atomic<uint32_t> observedId=0,observedArg=0;
    const auto child=t.create([&](uint32_t id,uint32_t arg){ observedId=t.currentId(); observedArg=arg; invoked=true; if(id!=observedId) throw std::runtime_error("wrong identity"); });
    require(child>main && !invoked,"created native worker waits for start");
    require(!t.status(child)->started,"dormant status");
    require(t.start(child,123)==child,"start worker");

    until([&]{ return t.status(child)->finished; },"worker completion");
    require(invoked && observedId==uint32_t(child) && observedArg==123,"callback native identity and argument");
    require(!t.status(child)->failed,"normal callback completion");
    require(t.setPriority(child,0x2C)==NativeThreads::Busy,"completed OS task cannot change scheduling priority");
    require(t.start(child,456)==child,"restart completed native worker under same logical id");
    until([&]{ return t.status(child)->finished; },"restarted worker completion");
    require(observedArg==456 && observedId==uint32_t(child),"restart gets fresh argument and same id");
    require(t.erase(child)==child && !t.status(child),"join and erase finished worker");
    require(t.erase(child)==NativeThreads::Unknown,"erased id not found");
    const auto dormant=t.create([](uint32_t,uint32_t){ throw std::runtime_error("must not execute"); });
    require(dormant>child,"ids not reused");
    require(t.erase(dormant)==dormant,"erase dormant cancels gated worker");
    std::atomic<int32_t> sleepResult=0;
    const auto waiter=t.create([&](uint32_t,uint32_t){ sleepResult=t.sleepCurrent(); });
    require(t.start(waiter,0)==waiter,"start sleeper");
    until([&]{ return t.status(waiter)->sleeping; },"actual native worker blocks");
    require(t.erase(waiter)==NativeThreads::Busy,"running worker cannot be erased");
    require(t.start(waiter,0)==NativeThreads::Busy && t.restart(waiter,0)==NativeThreads::Busy,"running task cannot restart");
    require(sleepResult==0,"sleep not completed without permit");
    require(t.wake(waiter)==waiter,"wake actual worker");
    until([&]{ return t.status(waiter)->finished; },"waiter wakes");
    require(sleepResult==waiter,"sleep returns current task id");
    require(t.wake(waiter)==NativeThreads::Busy,"finished task cannot wake");
    require(t.erase(waiter)==waiter,"erase sleeper after completion");
    std::atomic<int32_t> permitResult=0;
    const auto prewake=t.create([&](uint32_t,uint32_t){
        if(t.sleepCurrent()>0 && t.sleepCurrent()>0) permitResult=1;
    });
    require(t.wake(prewake)==prewake && t.wake(prewake)==prewake,"multiple prestart wake permits retained");
    require(t.status(prewake)->wakePermits==2,"permit count observable");
    t.start(prewake,0); until([&]{ return t.status(prewake)->finished; },"prewoken worker completed");
    require(permitResult==1 && t.status(prewake)->wakePermits==0,"exactly two permits consumed");
    require(t.erase(prewake)==prewake,"prewake cleanup");
    const auto failure=t.create([](uint32_t,uint32_t){ throw std::runtime_error("injected callback failure"); });
    t.start(failure,0); until([&]{ return t.status(failure)->finished; },"failed callback completion");
    require(t.status(failure)->failed,"callback exception recorded");
    require(t.erase(failure)==failure,"failed worker joined");
    std::atomic<int32_t> nested=0;
    const auto parent=t.create([&](uint32_t,uint32_t){
        const auto id=t.create([&](uint32_t own,uint32_t arg){ if(arg!=77 || own!=uint32_t(t.currentId())) throw std::runtime_error("nested identity"); });
        nested=t.start(id,77);
    });
    t.start(parent,0);
    until([&]{ return t.status(parent)->finished && nested>0 && t.status(nested)->finished; },"worker creates child");
    require(!t.status(nested)->failed,"nested actual native task");
    require(t.erase(parent)==parent && t.erase(nested)==nested,"nested cleanup");
    std::atomic<bool> foreignJoin=true; std::atomic<int32_t> foreignMain=0;
    std::thread foreign([&]{ foreignJoin=t.joinAll(); foreignMain=t.registerMain(); }); foreign.join();
    require(!foreignJoin && foreignMain==NativeThreads::Error,"owner-only main/join controls");
    const auto closedWaiter=t.create([&](uint32_t,uint32_t){ sleepResult=t.sleepCurrent(); });
    t.start(closedWaiter,0); until([&]{ return t.status(closedWaiter)->sleeping; },"closing waiter blocks");
    t.requestShutdown();
    require(t.stopRequested(),"shutdown flag visible");
    require(t.registerMain()==NativeThreads::Error,"main cannot register after shutdown");
    require(t.wake(main)==NativeThreads::Deleted && t.sleepCurrent()==NativeThreads::Deleted,"main wakes/sleeps fail after shutdown");
    require(t.start(closedWaiter,0)==NativeThreads::Deleted,"no start after shutdown");
    require(t.create([](uint32_t,uint32_t){})==NativeThreads::Error,"no new task after shutdown");
    require(t.joinAll(),"owner joins all after nonblocking shutdown");
    require(sleepResult==NativeThreads::Deleted,"shutdown releases actual blocked task");
    require(t.joinAll(),"join idempotent");
    require(t.currentId()==NativeThreads::Unknown,"main unbound after join");
    {
        NativeThreads limited(2); const auto m=limited.registerMain();
        const auto only=limited.create([](uint32_t,uint32_t){throw std::runtime_error("dormant never starts");});
        require(m>0 && only>m,"bounded manager initial tasks");
        require(limited.create([](uint32_t,uint32_t){})==NativeThreads::Error,"live capacity includesmain");
        require(limited.shutdown(),"shutdown joins dormant worker without running callback");
    }
    {
        NativeThreads retired(2); retired.registerMain();
        for(unsigned i=0;i<256;++i) {
            const auto id=retired.create([&](uint32_t own,uint32_t){
                if(retired.retireCurrent()!=int32_t(own)) throw std::runtime_error("self retire failed");
            });
            require(id>0,"completed self-delete does not exhaust lifetime task capacity");
            retired.start(id,0); until([&]{ return retired.status(id)->finished; },"retired callback completion");
            require(retired.status(id)->retired && !retired.status(id)->failed,"logical self-delete recorded");
            require(retired.start(id,0)==NativeThreads::Unknown,"deleted logical id cannot restart");
            require(retired.reapRetired() && !retired.status(id),"retired actual OS worker joined and removed");
        }
        require(retired.shutdown(),"retired manager shutdown");
    }
    require(t.shutdown(),"shutdown idempotent");
    std::printf("native threads: %u checks passed\n",checks);
}
