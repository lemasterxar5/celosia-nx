#pragma once
#include <cstdint>
#include <limits>
#include <optional>
namespace native_renderer::detail::deko {
// deko counter reports contain a value followed by the GPU timestamp.
struct GpuReport {uint64_t value,timestamp;};
static_assert(sizeof(GpuReport)==16);
inline std::optional<uint64_t> gpuCounterDelta(const GpuReport &begin,const GpuReport &end){
    // A zero delta cannot distinguish a supported idle counter from unsupported
    // hardware. Keep it unproven instead of publishing it as a measured zero.
    if(!begin.timestamp || end.timestamp<=begin.timestamp || end.value<=begin.value)return {};
    return end.value-begin.value;
}
struct GpuCounterValues {uint64_t vertices,fragments,primitives;};
struct GpuCounterWindow {
    uint64_t count=0,rejected=0,totalVertices=0,totalFragments=0,totalPrimitives=0;
    bool add(std::optional<GpuCounterValues> values){
        const auto maximum=std::numeric_limits<uint64_t>::max();
        if(!values || !values->vertices || !values->fragments || !values->primitives || count==maximum ||
           values->vertices>maximum-totalVertices || values->fragments>maximum-totalFragments ||
           values->primitives>maximum-totalPrimitives){
            if(rejected<maximum)++rejected;
            return false;
        }
        ++count;totalVertices+=values->vertices;totalFragments+=values->fragments;totalPrimitives+=values->primitives;
        return true;
    }
    double averageVertices()const{return count?double(totalVertices)/double(count):0;}
    double averageFragments()const{return count?double(totalFragments)/double(count):0;}
    double averagePrimitives()const{return count?double(totalPrimitives)/double(count):0;}
    double fragmentInvocationsPerPixel(uint64_t pixels)const{return pixels?averageFragments()/double(pixels):0;}
    void reset(){count=rejected=totalVertices=totalFragments=totalPrimitives=0;}
};
struct GpuTimeWindow {
    uint64_t count=0,totalNs=0,peakNs=0;
    bool add(uint64_t ns){
        if(ns>std::numeric_limits<uint64_t>::max()-totalNs || count==std::numeric_limits<uint64_t>::max())return false;
        ++count;totalNs+=ns;if(ns>peakNs)peakNs=ns;return true;
    }
    double averageNs()const{return count?double(totalNs)/double(count):0;}
    void reset(){count=totalNs=peakNs=0;}
};
inline std::optional<uint64_t> gpuIntervalNs(const GpuReport &begin,const GpuReport &end){
    if(!begin.timestamp || end.timestamp<begin.timestamp)return {};
    const auto ticks=end.timestamp-begin.timestamp;
    // Same units as dkTimestampToNs (625/384); split to avoid multiplication overflow.
    const auto whole=ticks/384,remainder=(ticks%384)*625/384;
    if(whole>(std::numeric_limits<uint64_t>::max()-remainder)/625)return {};
    return whole*625+remainder;
}
}
