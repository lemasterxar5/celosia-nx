#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
namespace celosia::native {
inline constexpr size_t AudioFrames=1024, AudioChannels=2;
inline constexpr size_t AudioSamples=AudioFrames*AudioChannels, AudioBytes=AudioSamples*sizeof(int16_t);
static_assert(AudioBytes==0x1000);
struct AudioPull { bool ok; size_t frames; };
template<class Source> AudioPull fillAudio(std::span<int16_t> buffer,Source &source) noexcept
{
    if(buffer.size()!=AudioSamples) return {false,0};
    std::fill(buffer.begin(),buffer.end(),0);
    try {
        const size_t count=source(buffer.data(),AudioFrames);
        if(count<=AudioFrames) {
            std::fill(buffer.begin()+count*AudioChannels,buffer.end(),0);
            return {true,count};
        }
    } catch(...) { /* Callback failure is reported to the output owner below. */ }
    std::fill(buffer.begin(),buffer.end(),0);
    return {false,0};
}
}
