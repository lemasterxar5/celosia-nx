#include "native_display.hpp"
#include "native_backend.h"
#include "native_executor.hpp"
#include <switch.h>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

#if !defined(__SWITCH__) || !defined(PS2X_NATIVE_DEKO3D)
#error "NativeDisplay requires the direct Switch deko3d backend"
#endif

namespace celosia::native {
namespace {
using NativeIdentity=uint64_t;
NativeIdentity nativeIdentity()
{
    NativeIdentity identity=0;
    if(R_FAILED(svcGetThreadId(&identity,CUR_THREAD_HANDLE)))
        throw std::runtime_error("Native display: cannot identify current OS thread");
    return identity;
}
}
struct NativeDisplay::Impl {
    NativeExecutor worker;
    std::unique_ptr<native_renderer::detail::Backend> gpu;
    NativeIdentity owner=0;
    bool frameOpen=false,failed=false;
    void requireOwner() const
    {
        if(gpu && nativeIdentity()!=owner)
            throw std::logic_error("Native display: operation on foreign OS thread");
    }
    void requireReady() const
    {
        requireOwner();
        if(!gpu || failed) throw std::logic_error("Native display: not initialized or failed");
    }
};
NativeDisplay::NativeDisplay() : impl(std::make_unique<Impl>()) {}
NativeDisplay::~NativeDisplay()
{
    try { impl->worker.execute([&] { impl->requireOwner(); impl->gpu.reset(); }); }
    catch(...) { std::fputs("Native display GPU cleanup failed\n",stderr); std::terminate(); }
}
void NativeDisplay::initialize()
{
    auto &s=*impl;
    s.worker.execute([&] {
        s.requireOwner();
        if(s.failed) throw std::logic_error("Native display: prior GPU operation failed");
        if(s.gpu) return;
        const auto identity=nativeIdentity();
        std::unique_ptr<native_renderer::detail::Backend> candidate(
            native_renderer::detail::createDeko3dBackend());
        if(!candidate) throw std::runtime_error("Native display: deko3d creation failed");
        candidate->beginFrame();candidate->endFrame(Width,Height);candidate->finishFrames();
        s.owner=identity;s.gpu=std::move(candidate);
    });
}
bool NativeDisplay::ready() const
{
    bool result=false;
    impl->worker.execute([&] { impl->requireOwner();result=impl->gpu && !impl->failed; });
    return result;
}
void NativeDisplay::beginFrame()
{
    auto &s=*impl;
    s.worker.execute([&] {
        s.requireReady();
        if(s.frameOpen) throw std::logic_error("Native display: frame already open");
        try { s.gpu->beginFrame(); s.frameOpen=true; }
        catch(...) { s.failed=true; throw; }
    });
}
void NativeDisplay::endFrame()
{
    auto &s=*impl;
    s.worker.execute([&] {
        s.requireReady();
        if(!s.frameOpen) throw std::logic_error("Native display: no frame open");
        try { s.gpu->endFrame(Width,Height); s.frameOpen=false; }
        catch(...) { s.failed=true; throw; }
    });
}
void NativeDisplay::synchronize()
{
    auto &s=*impl;
    s.worker.execute([&] {
        s.requireReady();
        if(s.frameOpen) throw std::logic_error("Native display: cannot synchronize an unfinished frame");
        try { s.gpu->finishFrames(); } catch(...) { s.failed=true;throw; }
    });
}
void NativeDisplay::submit(std::function<void(native_renderer::detail::Backend &)> operations)
{
    if(!operations) throw std::invalid_argument("Native display: empty submission");
    auto &s=*impl;
    s.worker.execute([&s,operations=std::move(operations)] {
        s.requireReady();
        try { operations(*s.gpu); } catch(...) { s.failed=true;throw; }
    });
}
}
