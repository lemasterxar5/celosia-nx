#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>
namespace native_renderer::detail::deko {
struct FrameRateSample {
    uint64_t frames,elapsedNs;double fps;
    // Per-presentation intervals inside the window (CPU submission cadence, not a
    // compositor timestamp). worstFrameNs is the longest gap between two consecutive
    // presentations; low1FrameNs is the mean of the slowest 1% (at least one) of them.
    // Both are zero when fewer than two presentations occurred.
    uint32_t intervalCount;uint64_t worstFrameNs,low1FrameNs;
    double low1Fps()const{return low1FrameNs?1000000000.0/double(low1FrameNs):0;}
    double worstFps()const{return worstFrameNs?1000000000.0/double(worstFrameNs):0;}
};
class FrameRateWindow {
    static constexpr size_t MaxIntervals=4096; // ~5 s at 60 FPS is 300; bounded for safety
    uint64_t start=0,frames=0,lastPresented=0,worst=0;
    bool initialized=false,havePresented=false;
    std::vector<uint64_t> intervals;
public:
    FrameRateWindow(){intervals.reserve(512);}
    std::optional<FrameRateSample> record(uint64_t now,bool presented){
        if(!initialized || now<start){
            initialized=true;start=now;frames=0;worst=0;intervals.clear();
            havePresented=presented;lastPresented=now;return {};
        }
        if(presented){
            ++frames;
            if(havePresented && now>=lastPresented){
                const auto gap=now-lastPresented;
                worst=std::max(worst,gap);
                if(intervals.size()<MaxIntervals)intervals.push_back(gap);
            }
            havePresented=true;lastPresented=now;
        }
        const auto elapsed=now-start;
        if(elapsed<5000000000ull)return {};
        FrameRateSample sample{frames,elapsed,double(frames)*1000000000.0/double(elapsed),
            uint32_t(intervals.size()),worst,0};
        if(!intervals.empty()){
            const size_t slow=std::max<size_t>(1,(intervals.size()+99)/100);
            std::nth_element(intervals.begin(),intervals.begin()+(intervals.size()-slow),intervals.end());
            uint64_t sum=0;for(size_t i=intervals.size()-slow;i<intervals.size();++i)sum+=intervals[i];
            sample.low1FrameNs=sum/slow;
        }
        start=now;frames=0;worst=0;intervals.clear();return sample;
    }
};
}
