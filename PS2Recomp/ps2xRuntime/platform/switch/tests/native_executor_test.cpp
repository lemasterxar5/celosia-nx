#include "native_executor.hpp"
#include "native_threads.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

static std::atomic<unsigned> checks=0;
static void require(bool ok,const char *why) { ++checks;if(!ok) throw std::runtime_error(why); }
struct JobFailure : std::runtime_error {using std::runtime_error::runtime_error;};
#ifdef CELOSIA_EXECUTOR_TEST_WAIT_FAILURE
namespace celosia::native::executor_test {
void failNextCompletionWait();
unsigned completionWaitFailuresSeen();
}
#endif
int main()
{
    using namespace celosia::native;
    NativeThreads game;
    const auto main=game.registerMain();require(main>0,"game main bound to actual owner thread");
    const auto creator=std::this_thread::get_id();
    std::thread::id worker,createdOn,destroyedOn;
    struct Resource {
        std::thread::id &destroyed;
        explicit Resource(std::thread::id &out):destroyed(out){}
        ~Resource(){destroyed=std::this_thread::get_id();}
    };
    std::unique_ptr<Resource> resource;
    {
        NativeExecutor executor;
        require(game.currentId()==main,"executor construction preserves existing game TLS identity");
        executor.execute([&]{worker=std::this_thread::get_id();createdOn=worker;resource=std::make_unique<Resource>(destroyedOn);require(game.currentId()==NativeThreads::Unknown,"render worker does not masquerade as game thread");});
        require(worker!=creator && createdOn==worker,"job runs on a real dedicated OS thread");
        bool thrown=false;
        try{executor.execute([]{throw JobFailure("exact job error");});}catch(const JobFailure &error){thrown=std::string(error.what())=="exact job error";}
        require(thrown,"original typed job exception reaches caller");
        executor.execute([&]{require(std::this_thread::get_id()==worker,"worker survives throwing job");});
        thrown=false;try{executor.execute([]{throw 42;});}catch(int error){thrown=error==42;}
        require(thrown,"nonstandard job exception also propagated");
        thrown=false;try{executor.execute({});}catch(const std::invalid_argument &){thrown=true;}
        require(thrown,"empty job rejected");
        thrown=false;try{executor.execute([&]{executor.execute([]{});});}catch(const std::logic_error &){thrown=true;}
        require(thrown,"nested execute rejected before lock deadlock");
        thrown=false;try{executor.execute([&]{executor.shutdown();});}catch(const std::logic_error &){thrown=true;}
        require(thrown,"worker cannot shut down or join itself");
        bool foreignShutdownRejected=false;
        std::thread foreign([&]{try{executor.shutdown();}catch(const std::logic_error &){foreignShutdownRejected=true;}});foreign.join();
        require(foreignShutdownRejected,"shutdown restricted to constructing OS thread");
#ifdef CELOSIA_EXECUTOR_TEST_WAIT_FAILURE
        std::atomic<bool> entered=false,release=false,returned=false,capturesDestroyed=false;
        bool waitError=false;std::thread::id captureDestroyedOn;
        struct Capture {
            std::atomic<bool> &destroyed;std::thread::id &where;
            Capture(std::atomic<bool> &flag,std::thread::id &thread):destroyed(flag),where(thread){}
            ~Capture(){where=std::this_thread::get_id();destroyed=true;}
        };
        executor_test::failNextCompletionWait();
        const auto faultCaller=game.create([&](uint32_t,uint32_t){
            try {
                executor.execute([&,keep=std::make_shared<Capture>(capturesDestroyed,captureDestroyedOn)]{
                    entered=true;while(!release.load()) std::this_thread::yield();
                });
            } catch(const std::runtime_error &error) {
                waitError=std::string(error.what())=="Native executor: completion wait failed";
            }
            require(capturesDestroyed,"execute cannot throw until job captures destroyed");
            returned=true;
        });
        require(faultCaller>0 && game.start(faultCaller,0)==faultCaller,"wait-fault native caller starts");
        const auto faultDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while((!entered || executor_test::completionWaitFailuresSeen()==0) && std::chrono::steady_clock::now()<faultDeadline)
            std::this_thread::yield();
        const bool faultObserved=entered && executor_test::completionWaitFailuresSeen()==1;
        const bool held=!returned;
        release=true; // Always release before assertions/unwinding owner state.
        require(faultObserved && held,"abnormal completion wait keeps caller blocked while job runs");
        while(!game.status(faultCaller)->finished) {
            if(std::chrono::steady_clock::now()>=faultDeadline) throw std::runtime_error("wait-fault caller timed out");
            std::this_thread::yield();
        }
        require(!game.status(faultCaller)->failed && returned && waitError,"native wait failure surfaces only after safe completion");
        require(captureDestroyedOn==worker && game.erase(faultCaller)==faultCaller,"faulted job capture cleaned on worker and caller joined");
        executor.execute([&]{require(std::this_thread::get_id()==worker,"cleanup job works after wait fault and completion drain");});
#endif
        std::atomic<unsigned> active=0,total=0;
        std::array<int32_t,8> callers{};
        for(auto &id:callers) {
            id=game.create([&](uint32_t own,uint32_t){
                for(unsigned i=0;i<32;++i) {
                    executor.execute([&]{
                        require(std::this_thread::get_id()==worker,"every caller uses same render OS thread");
                        require(active.fetch_add(1)==0,"jobs never execute concurrently");
                        std::this_thread::yield();++total;
                        require(active.fetch_sub(1)==1,"one serialized active job");
                    });
                    require(game.currentId()==int32_t(own),"native caller identity preserved after execute");
                }
            });
            require(id>0 && game.start(id,0)==id,"real native caller starts");
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        for(const auto id:callers) {
            while(!game.status(id)->finished) {
                if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("executor caller timed out");
                std::this_thread::yield();
            }
            require(!game.status(id)->failed,"concurrent native caller completed without lost wake/error");
            require(game.erase(id)==id,"finished native caller joined");
        }
        require(total==256 && active==0,"all eight caller batches completed synchronously");
        executor.execute([&]{resource.reset();});
        require(destroyedOn==worker,"cleanup runs on creating render worker after errors");
        require(game.currentId()==main,"game main identity preserved after jobs");
        executor.shutdown();executor.shutdown();
        thrown=false;try{executor.execute([]{});}catch(const std::logic_error &){thrown=true;}
        require(thrown,"new jobs reject after shutdown");
    }
    require(game.currentId()==main,"executor shutdown/destructor preserves game main TLS binding");
    {
        NativeExecutor idle; // Destructor must wake and join its idle OS worker.
    }
    require(game.currentId()==main,"idle executor destructor does not steal main registration");
    require(game.shutdown(),"game registry remains independently functional");
    std::printf("native executor: %u checks passed\n",checks.load());
}
