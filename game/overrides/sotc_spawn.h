// v117: a detached host thread that also works on the Switch. libnx's std::thread::detach throws
// (pthread_detach answers ENOSYS) and that terminates the program (v97, v106): pthread_create there,
// as the hook worker does, and the ENOSYS answer ignored (the thread runs for the program's life).
#pragma once
#include <cstddef>
#include <pthread.h>
#include <stdexcept>
#include <thread>

namespace sotc_spawn
{
    inline void detached(void (*fn)(), std::size_t stack = 512u * 1024u)
    {
#if defined(__SWITCH__)
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stack);
        pthread_t worker;
        const int created = pthread_create(
            &worker, &attr, [](void *f) -> void * { reinterpret_cast<void (*)()>(f)(); return nullptr; }, reinterpret_cast<void *>(fn));
        pthread_attr_destroy(&attr);
        if (created != 0)
            throw std::runtime_error("worker thread creation failed");
        pthread_detach(worker);
#else
        (void)stack;
        std::thread(fn).detach();
#endif
    }
}
