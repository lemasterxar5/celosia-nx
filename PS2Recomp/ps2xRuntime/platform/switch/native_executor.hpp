#pragma once
#include <functional>
#include <memory>

namespace celosia::native {
// One real OS worker owns native rendering resources. Calls from other native
// threads serialize and wait synchronously; job errors return to their caller
// without stopping the worker, so a later cleanup job can release GPU objects.
// Caller holds required data/execution gates until execute returns. A job must
// not reacquire those caller-held gates or wait for its blocked caller.
// Do not begin new calls concurrently with destruction. Join all callers first,
// submit resource cleanup, then destroy/shutdown on the constructing OS thread.
class NativeExecutor final {
public:
    NativeExecutor();
    ~NativeExecutor();
    NativeExecutor(const NativeExecutor &)=delete;
    NativeExecutor &operator=(const NativeExecutor &)=delete;
    // Empty jobs and nested execution on this worker reject explicitly. The
    // callback and its captures finish/destruct before the caller is released.
    void execute(std::function<void()> job);
    // Asynchronous hand-over: returns once the worker owns the job. Every later
    // execute/post/drain first waits for it and rethrows its error, so at most one
    // job is in flight. Captures must own their data (nothing caller-backed).
    void post(std::function<void()> job);
    // Waits for a posted job (no-op without one); rethrows its error.
    void drain();
    // Creator-only, synchronous and idempotent. Wakes the idle worker and joins
    // it; subsequent jobs reject. Destructor performs this if still needed.
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
