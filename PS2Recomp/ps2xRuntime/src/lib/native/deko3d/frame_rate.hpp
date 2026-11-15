#pragma once
#include <cstdint>
#include <optional>
namespace native_renderer::detail::deko {
struct FrameRateSample {uint64_t frames,elapsedNs;double fps;};
class FrameRateWindow {
    uint64_t start=0,frames=0;
    bool initialized=false;
public:
    std::optional<FrameRateSample> record(uint64_t now,bool presented){
        if(!initialized || now<start){initialized=true;start=now;frames=0;return {};}
        if(presented)++frames;
        const auto elapsed=now-start;
        if(elapsed<5000000000ull)return {};
        FrameRateSample sample{frames,elapsed,double(frames)*1000000000.0/double(elapsed)};
        start=now;frames=0;return sample;
    }
};
}
