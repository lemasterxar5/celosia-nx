#include "native_executor.hpp"
#include "native_threads.hpp"
#include "native_semaphore.hpp"
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>
#ifdef CELOSIA_EXECUTOR_TEST_WAIT_FAILURE
#include <atomic>
#endif
#ifdef __SWITCH__
extern "C" {
#include <switch.h>
}
#else
#include <chrono>
#include <thread>
#endif

namespace celosia::native {
namespace {
#ifdef __SWITCH__
using NativeIdentity=uint64_t;
NativeIdentity nativeIdentity()
{
    NativeIdentity identity=0;
    if(R_FAILED(svcGetThreadId(&identity,CUR_THREAD_HANDLE)))
        throw std::runtime_error("Native executor: cannot identify current OS thread");
    return identity;
}
#else
using NativeIdentity=std::thread::id;
NativeIdentity nativeIdentity() {return std::this_thread::get_id();}
#endif
void waitCold() noexcept
{
#ifdef __SWITCH__
    (void)svcSleepThread(1000000);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}
#ifdef CELOSIA_EXECUTOR_TEST_WAIT_FAILURE
std::atomic<bool> failWait{false};
std::atomic<unsigned> failedWaits{0};
#endif
}
#ifdef CELOSIA_EXECUTOR_TEST_WAIT_FAILURE
// Private fault-test symbols; absent from release builds and public headers.
namespace executor_test {
void failNextCompletionWait() {failWait.store(true);}
unsigned completionWaitFailuresSeen() {return failedWaits.load();}
}
#endif
struct NativeExecutor::Impl {
    const NativeIdentity owner=nativeIdentity();
    NativeThreads threads{1};
    SemaphoreSet semaphores;
    int32_t work=0,done=0,worker=0;
    std::mutex serialized,state;
    std::function<void()> queued;
    std::exception_ptr result,infrastructureFailure;
    bool closing=false,closed=false,outstanding=false,posted=false;

    ~Impl()
    {
        try {close();}
        catch(...) {std::fputs("Native executor: worker could not be joined safely\n",stderr);std::terminate();}
    }
    void initialize()
    {
        work=semaphores.create(0,1,0,0);done=semaphores.create(0,1,0,0);
        if(work<=0 || done<=0) throw std::runtime_error("Native executor: semaphore creation failed");
        // Do not registerMain(): this creator can already own the game's TLS
        // identity. The worker has its own real NativeThreads task binding.
        worker=threads.create([this](uint32_t,uint32_t){run();},0x2C,0x100000);
        if(worker<=0) throw std::runtime_error("Native executor: OS worker creation failed");
        if(threads.start(worker,0)!=worker) throw std::runtime_error("Native executor: OS worker start failed");
    }
    void loop()
    {
        for(;;) {
            const auto wake=semaphores.wait(work);
            std::function<void()> job;
            {
                std::lock_guard lock(state);
                if(closing) return;
                if(wake!=work || !queued) throw std::runtime_error("Native executor: work wake failed");
                job=std::move(queued);
            }
            std::exception_ptr error;
            try {job();}catch(...) {error=std::current_exception();}
            // Destroy job captures on the owning worker before completion.
            job={};
            {
                std::lock_guard lock(state);
                result=std::move(error);
                outstanding=false;
                if(semaphores.signal(done)!=done)
                    throw std::runtime_error("Native executor: completion signal failed");
            }
        }
    }
    void run() noexcept
    {
        try {loop();}
        catch(...) {
            {
                std::lock_guard lock(state);
                infrastructureFailure=std::current_exception();
                queued={};
                outstanding=false;
            }
            // An infrastructure failure cannot leave a synchronous caller
            // waiting forever. No callback remains running on this path.
            semaphores.shutdown();
        }
    }
    int32_t completionWait()
    {
#ifdef CELOSIA_EXECUTOR_TEST_WAIT_FAILURE
        if(failWait.exchange(false)) {++failedWaits;return SemaphoreSet::Error;}
#endif
        return semaphores.wait(done);
    }
    void waitForRelease()
    {
        for(;;) {
            {std::lock_guard lock(state);if(!outstanding) return;}
            // Only reached after an abnormal native wait or stale permit. Do
            // not return/throw while worker still owns caller-backed captures.
            waitCold();
        }
    }
    // Hands the job to the worker under the serialized lock (caller holds it).
    void submit(std::function<void()> job)
    {
        std::lock_guard lock(state);
        if(closing || closed) throw std::logic_error("Native executor: already shut down");
        if(infrastructureFailure) std::rethrow_exception(infrastructureFailure);
        result=nullptr;queued=std::move(job);outstanding=true;
        // Worker cannot claim the job until signal succeeds and this state
        // lock is released. On failure it sees no caller-owned callback.
        if(semaphores.signal(work)!=work) {
            queued={};outstanding=false;throw std::runtime_error("Native executor: work signal failed");
        }
    }
    // Waits for the submitted job and returns its error (caller holds the serialized lock).
    std::exception_ptr complete()
    {
        const auto completion=completionWait();
        waitForRelease();
        std::lock_guard lock(state);
        if(infrastructureFailure) std::rethrow_exception(infrastructureFailure);
        const auto remaining=semaphores.poll(done);
        if(remaining!=done && remaining!=SemaphoreSet::Empty)
            throw std::runtime_error("Native executor: completion drain failed");
        if(completion!=done) throw std::runtime_error("Native executor: completion wait failed");
        return std::exchange(result,nullptr);
    }
    // A posted job still running (or finished, not yet collected) completes before any other
    // call proceeds: at most one job is ever in flight, and its error reaches the next caller.
    void collectPosted()
    {
        if(!posted) return;
        posted=false;
        if(auto error=complete()) std::rethrow_exception(error);
    }
    void execute(std::function<void()> job)
    {
        if(threads.currentId()==worker) throw std::logic_error("Native executor: nested worker execute rejected");
        if(!job) throw std::invalid_argument("Native executor: empty job");
        std::lock_guard call(serialized);
        collectPosted();
        submit(std::move(job));
        if(auto error=complete()) std::rethrow_exception(error);
    }
    void post(std::function<void()> job)
    {
        if(threads.currentId()==worker) throw std::logic_error("Native executor: nested worker post rejected");
        if(!job) throw std::invalid_argument("Native executor: empty job");
        std::lock_guard call(serialized);
        collectPosted();
        submit(std::move(job));
        posted=true;
    }
    void drain()
    {
        if(threads.currentId()==worker) throw std::logic_error("Native executor: nested worker drain rejected");
        std::lock_guard call(serialized);
        collectPosted();
    }
    void close()
    {
        if(nativeIdentity()!=owner) throw std::logic_error("Native executor: shutdown requires creator OS thread");
        std::lock_guard call(serialized);
        // A posted job finishes (its captures released on the worker) before shutdown.
        if(posted) {posted=false;try{(void)complete();}catch(...){}}
        {
            std::lock_guard lock(state);
            if(closed) return;
            closing=true;
        }
        threads.requestShutdown();
        semaphores.shutdown();
        if(!threads.joinAll()) throw std::runtime_error("Native executor: OS worker join failed");
        std::lock_guard lock(state);closed=true;
    }
};
NativeExecutor::NativeExecutor() : impl(std::make_unique<Impl>()) {impl->initialize();}
NativeExecutor::~NativeExecutor()=default;
void NativeExecutor::execute(std::function<void()> job) {impl->execute(std::move(job));}
void NativeExecutor::post(std::function<void()> job) {impl->post(std::move(job));}
void NativeExecutor::drain() {impl->drain();}
void NativeExecutor::shutdown() {impl->close();}
}
