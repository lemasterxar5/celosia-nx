#include "native_audio_policy.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace celosia::native;
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    std::array<int16_t,AudioSamples> samples;
    samples.fill(55);
    auto shortSource=[](int16_t *data,size_t frames) { if(frames!=AudioFrames) throw std::runtime_error("wrong frame unit");
        std::fill(data,data+AudioSamples,33); data[0]=12; return size_t(3); };
    const auto shortPull=fillAudio(samples,shortSource);
    require(shortPull.ok && shortPull.frames==3,"frame count is stereo frames");
    require(samples[0]==12 && samples[5]==33 && samples[6]==0 && samples.back()==0,"short pull tail is silence");
    auto empty=[](int16_t *,size_t) { return size_t(0); };
    require(fillAudio(samples,empty).ok,"empty source is valid silence");
    require(std::all_of(samples.begin(),samples.end(),[](auto sample){return sample==0;}),"silence has no stale sample");
    auto full=[](int16_t *data,size_t frames) { std::fill(data,data+frames*2,-25); return frames; };
    require(fillAudio(samples,full).frames==AudioFrames && samples.back()==-25,"full PCM pull");
    auto tooMany=[](int16_t *data,size_t) { data[0]=111; return SIZE_MAX; };
    require(!fillAudio(samples,tooMany).ok && samples[0]==0 && samples.back()==0,"invalid reported size silenced");
    auto throws=[](int16_t *data,size_t)->size_t { data[0]=77; throw std::runtime_error("source failed"); };
    require(!fillAudio(samples,throws).ok && samples[0]==0,"source exception cannot leak stale output");
    std::array<int16_t,3> bad{1,2,3}; bool called=false;
    auto mustNotCall=[&](int16_t *,size_t) { called=true; return size_t(0); };
    require(!fillAudio(bad,mustNotCall).ok && !called && bad==std::array<int16_t,3>{1,2,3},"wrong buffer rejected before source");
    std::printf("native audio policy: %u checks passed\n",checks);
}
