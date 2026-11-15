#pragma once
// GPU time by draw category on counter-sampled frames: a timestamp is reported whenever the
// category of the next GPU work changes, and the interval up to the next timestamp is charged
// to the category that preceded it. Timestamps are pipelined, so a single interval can include
// overlap with neighbouring work; the per-category sums are a guide to where frame time goes,
// and they add up to the whole recorded interval.
#include "gpu_timing.hpp"
#include <array>
#include <cstdint>
namespace native_renderer::detail::deko {
enum class GpuSegment : uint8_t {Prepass,Backdrop,Opaque,Generic,Failure,Shadow,Screen,Count};
inline constexpr const char *gpuSegmentNames="prepass,backdrop,opaque,generic,failure,shadow,screen";
constexpr unsigned GpuSegmentCount=unsigned(GpuSegment::Count);
constexpr unsigned MaxGpuSegments=2048; // reports per sampled frame (16 bytes each)

struct GpuSegmentRecorder {
    std::array<uint8_t,MaxGpuSegments> kinds{};
    unsigned count=0;
    int current=-1;
    bool active=false;
    void begin(){count=0;current=-1;}
    // Returns the report slot to write a timestamp to, or -1 when no timestamp is needed.
    int change(GpuSegment kind){
        if(!active || int(kind)==current || count+1>=MaxGpuSegments)return -1;
        current=int(kind);kinds[count]=uint8_t(kind);return int(count++);
    }
    // Closing timestamp (charged to nothing).
    int close(){if(!active || current<0 || count>=MaxGpuSegments)return -1;current=-1;kinds[count]=uint8_t(GpuSegment::Count);return int(count++);}
};

struct GpuSegmentWindow {
    std::array<uint64_t,GpuSegmentCount> totalNs{};
    uint64_t frames=0;
    // reports[i] starts segment kinds[i]; the last report only closes the previous one.
    template<class Reports>
    bool add(const Reports &reports,const uint8_t *kinds,unsigned count){
        if(count<2)return false;
        std::array<uint64_t,GpuSegmentCount> frame{};
        for(unsigned i=0;i+1<count;++i){
            if(kinds[i]>=GpuSegmentCount)return false;
            const auto ns=gpuIntervalNs(reports[i],reports[i+1]);
            if(!ns)return false;
            frame[kinds[i]]+=*ns;
        }
        for(unsigned k=0;k<GpuSegmentCount;++k)totalNs[k]+=frame[k];
        ++frames;return true;
    }
    double averageMs(unsigned kind)const{return frames?double(totalNs[kind])/double(frames)/1e6:0;}
    void reset(){totalNs.fill(0);frames=0;}
};
}
