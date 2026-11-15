#include "native_audio_output.hpp"
#include "native_audio_policy.hpp"
#include <switch.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <malloc.h>
namespace celosia::native {
namespace { std::atomic<bool> deviceOwned{false}; }
struct NativeAudioOutput::Impl {
    std::array<AudioOutBuffer,3> buffers{};
    std::array<bool,3> queued{};
    Source source;
    Thread thread{};
    bool owned=false,initialized=false,deviceStarted=false,created=false,started=false;
    std::atomic<bool> stopRequested{false},running{false};
    std::atomic<uint64_t> failure{0},submitted{0},completed{0};
    void fail(AudioFailure kind,Result result=0) noexcept
    {
        uint64_t expected=0;
        failure.compare_exchange_strong(expected,(uint64_t(kind)<<32)|result,std::memory_order_relaxed);
    }
    bool fill(AudioOutBuffer &buffer)
    {
        if(fillAudio(std::span<int16_t>(static_cast<int16_t *>(buffer.buffer),AudioSamples),source).ok) return true;
        fail(AudioFailure::Source); return false;
    }
    bool submit(size_t index)
    {
        const Result rc=audoutAppendAudioOutBuffer(&buffers[index]);
        if(R_FAILED(rc)) { fail(AudioFailure::Submit,rc); return false; }
        queued[index]=true; submitted.fetch_add(AudioFrames,std::memory_order_relaxed); return true;
    }
    static void entry(void *argument) { static_cast<Impl *>(argument)->run(); }
    void run() noexcept
    {
        while(!stopRequested.load(std::memory_order_acquire)) {
            AudioOutBuffer *released=nullptr; u32 count=0;
            const Result rc=audoutWaitPlayFinish(&released,&count,50000000);
            if(rc==KERNELRESULT(TimedOut)) continue;
            if(R_FAILED(rc)) { fail(AudioFailure::Wait,rc); break; }
            if(!count) continue;
            size_t index=0;
            while(index<buffers.size() && released!=&buffers[index]) ++index;
            // libnx's IPC output buffer holds exactly one pointer, not a list.
            if(count!=1 || index==buffers.size() || !queued[index]) { fail(AudioFailure::ReleasedBuffer); break; }
            queued[index]=false; completed.fetch_add(1,std::memory_order_relaxed);
            if(stopRequested.load(std::memory_order_acquire)) break;
            if(!fill(buffers[index]) || !submit(index)) break;
        }
        running.store(false,std::memory_order_release);
    }
};
NativeAudioOutput::NativeAudioOutput() : impl(std::make_unique<Impl>()) {}
NativeAudioOutput::~NativeAudioOutput() { stop(); }
bool NativeAudioOutput::start(Source source)
{
    auto &s=*impl;
    if(s.owned || s.created) return false;
    s.failure.store(0); s.submitted.store(0); s.completed.store(0); s.stopRequested.store(false);
    if(!source) { s.fail(AudioFailure::InvalidSource); return false; }
    bool expected=false;
    if(!deviceOwned.compare_exchange_strong(expected,true)) { s.fail(AudioFailure::DeviceBusy); return false; }
    s.owned=true; s.source=std::move(source);
    Result rc=audoutInitialize();
    if(R_FAILED(rc)) { s.fail(AudioFailure::Initialize,rc); stop(); return false; }
    s.initialized=true;
    if(audoutGetSampleRate()!=48000 || audoutGetChannelCount()!=2 || audoutGetPcmFormat()!=PcmFormat_Int16) {
        s.fail(AudioFailure::Format); stop(); return false;
    }
    for(auto &buffer:s.buffers) {
        buffer={}; buffer.buffer=memalign(0x1000,AudioBytes);
        buffer.buffer_size=AudioBytes; buffer.data_size=AudioBytes;
        if(!buffer.buffer) { s.fail(AudioFailure::Allocate); stop(); return false; }
        if(!s.fill(buffer)) { stop(); return false; }
    }
    rc=audoutStartAudioOut();
    if(R_FAILED(rc)) { s.fail(AudioFailure::DeviceStart,rc); stop(); return false; }
    s.deviceStarted=true;
    for(size_t i=0;i<s.buffers.size();++i) if(!s.submit(i)) { stop(); return false; }
    s.thread={};
    rc=threadCreate(&s.thread,Impl::entry,&s,nullptr,0x20000,0x2B,-2);
    if(R_FAILED(rc)) { s.fail(AudioFailure::ThreadCreate,rc); stop(); return false; }
    s.created=true; s.running.store(true,std::memory_order_release);
    rc=threadStart(&s.thread);
    if(R_FAILED(rc)) { s.running.store(false); s.fail(AudioFailure::ThreadStart,rc); stop(); return false; }
    s.started=true;
    return true;
}
void NativeAudioOutput::stop() noexcept
{
    auto &s=*impl;
    s.stopRequested.store(true,std::memory_order_release);
    if(s.started) {
        // Returning with a live callback would violate source/buffer lifetimes.
        const Result rc=threadWaitForExit(&s.thread);
        if(R_FAILED(rc)) { std::fprintf(stderr,"Native audio join failed: 0x%x\n",rc); std::terminate(); }
        s.started=false;
    }
    if(s.created) {
        const Result rc=threadClose(&s.thread);
        if(R_FAILED(rc)) s.fail(AudioFailure::ThreadClose,rc);
        s.created=false;
    }
    if(s.deviceStarted) {
        const Result rc=audoutStopAudioOut();
        if(R_FAILED(rc)) s.fail(AudioFailure::DeviceStop,rc);
        s.deviceStarted=false;
    }
    if(s.initialized) { audoutExit(); s.initialized=false; }
    for(auto &buffer:s.buffers) { std::free(buffer.buffer); buffer={}; }
    s.queued.fill(false); s.source=nullptr; s.running.store(false);
    if(s.owned) { s.owned=false; deviceOwned.store(false,std::memory_order_release); }
}
AudioOutputStats NativeAudioOutput::stats() const noexcept
{
    const auto &s=*impl;
    const bool active=s.running.load(std::memory_order_acquire);
    const auto error=s.failure.load(std::memory_order_relaxed);
    return {active,AudioFailure(error>>32),uint32_t(error),
        s.submitted.load(std::memory_order_relaxed),s.completed.load(std::memory_order_relaxed)};
}
}
