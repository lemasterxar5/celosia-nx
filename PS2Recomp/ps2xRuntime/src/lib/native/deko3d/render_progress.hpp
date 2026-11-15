#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
namespace native_renderer::detail::deko {
enum class RenderPhase {Idle,CpuFrame,Dispatch,Fence,Uploads,Acquire,Record,Submit,Present,Flush,Failed};
inline std::atomic<RenderPhase> renderPhase{RenderPhase::Idle};
inline std::atomic<uint64_t> renderSince{0};
inline uint64_t progressClock(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void markRender(RenderPhase phase){renderPhase.store(phase,std::memory_order_relaxed);renderSince.store(progressClock(),std::memory_order_release);}
inline const char *renderPhaseName(RenderPhase phase){
    switch(phase){
    case RenderPhase::Idle:return "idle";case RenderPhase::CpuFrame:return "cpu-frame";
    case RenderPhase::Dispatch:return "dispatch";case RenderPhase::Fence:return "frame-fence";
    case RenderPhase::Uploads:return "uploads";case RenderPhase::Acquire:return "acquire-image";
    case RenderPhase::Record:return "record-draws";case RenderPhase::Submit:return "submit";
    case RenderPhase::Present:return "present";case RenderPhase::Flush:return "flush";
    case RenderPhase::Failed:return "failed";
    }return "unknown";
}
}
