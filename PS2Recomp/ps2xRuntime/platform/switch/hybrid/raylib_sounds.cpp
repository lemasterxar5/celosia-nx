#include "host_declarations.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>
namespace {
constexpr size_t MaxBytes=32u*1024u*1024u;
struct Voice {std::vector<int16_t> samples;uint32_t rate,channels;double position=0,pitch=1,volume=1;bool playing=false;};
struct Sounds {std::mutex mutex;std::unordered_map<uintptr_t,Voice> voices;uintptr_t next=1;size_t bytes=0;};
Sounds &sounds()
{
    // Synchronization has process lifetime: cross-TU static destruction must
    // never destroy this registry before the native output thread is joined.
    // clearSwitchSounds releases all PCM storage after CloseAudioDevice joins.
    static auto *state=new Sounds;return *state;
}
Voice &lookup(Sound sound)
{
    auto &s=sounds();const auto it=s.voices.find(reinterpret_cast<uintptr_t>(sound.stream.buffer));
    if(sound.stream.processor!=&s || it==s.voices.end())throw std::invalid_argument("Invalid Switch sound handle");
    return it->second;
}
uint32_t read32(const unsigned char *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
uint16_t read16(const unsigned char *p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
}
Sound LoadSoundFromWave(Wave wave)
{
    if(!wave.data || !wave.frameCount || wave.sampleSize!=16 || (wave.channels!=1 && wave.channels!=2) ||
       wave.sampleRate<8000 || wave.sampleRate>192000 || uint64_t(wave.frameCount)*wave.channels*2>MaxBytes)
        throw std::invalid_argument("Unsupported native PCM sound format");
    const auto count=size_t(wave.frameCount)*wave.channels;auto &s=sounds();std::lock_guard lock(s.mutex);
    if(s.voices.size()>=128 || s.bytes>MaxBytes-count*2 || s.next==std::numeric_limits<uintptr_t>::max())
        throw std::runtime_error("Native one-shot sound capacity exceeded");
    const auto *input=static_cast<const int16_t *>(wave.data);const auto id=s.next++;
    s.voices.emplace(id,Voice{{input,input+count},wave.sampleRate,wave.channels});s.bytes+=count*2;
    return {{reinterpret_cast<void *>(id),&s,wave.sampleRate,16,wave.channels},wave.frameCount};
}
bool IsSoundPlaying(Sound sound){auto &s=sounds();std::lock_guard lock(s.mutex);return lookup(sound).playing;}
void PlaySound(Sound sound){auto &s=sounds();std::lock_guard lock(s.mutex);auto &v=lookup(sound);v.position=0;v.playing=true;}
void StopSound(Sound sound){auto &s=sounds();std::lock_guard lock(s.mutex);lookup(sound).playing=false;}
void UnloadSound(Sound sound)
{auto &s=sounds();std::lock_guard lock(s.mutex);s.bytes-=lookup(sound).samples.size()*2;s.voices.erase(reinterpret_cast<uintptr_t>(sound.stream.buffer));}
void SetSoundPitch(Sound sound,float pitch)
{if(!std::isfinite(pitch)||pitch<=0||pitch>16)throw std::invalid_argument("Invalid native sound pitch");auto &s=sounds();std::lock_guard lock(s.mutex);lookup(sound).pitch=pitch;}
void SetSoundVolume(Sound sound,float volume)
{if(!std::isfinite(volume)||volume<0||volume>1)throw std::invalid_argument("Invalid native sound volume");auto &s=sounds();std::lock_guard lock(s.mutex);lookup(sound).volume=volume;}
void mixSwitchSounds(int16_t *output,size_t frames)
{
    auto &s=sounds();std::lock_guard lock(s.mutex);
    for(auto &[id,v]:s.voices) {
        if(!v.playing)continue;const auto count=v.samples.size()/v.channels;
        for(size_t frame=0;frame<frames && v.position<count;++frame) {
            const auto index=static_cast<size_t>(v.position),next=std::min(index+1,count-1);const double fraction=v.position-index;
            for(unsigned channel=0;channel<2;++channel){const auto selected=v.channels==1?0:channel;
                const double sample=(v.samples[index*v.channels+selected]*(1-fraction)+v.samples[next*v.channels+selected]*fraction)*v.volume;
                const auto mixed=int32_t(output[frame*2+channel])+int32_t(std::lround(sample));
                output[frame*2+channel]=int16_t(std::clamp(mixed,-32768,32767));}
            v.position+=double(v.rate)*v.pitch/48000.0;
        }
        if(v.position>=count)v.playing=false;
    }
}
void clearSwitchSounds(){auto &s=sounds();std::lock_guard lock(s.mutex);s.voices.clear();s.bytes=0;}
Wave LoadWaveFromMemory(const char *type,const unsigned char *data,int size)
{
    if(!type || std::strcmp(type,".wav") || !data || size<12 || size>int(MaxBytes) ||
       std::memcmp(data,"RIFF",4) || std::memcmp(data+8,"WAVE",4) || uint64_t(read32(data+4))+8>uint64_t(size))
        throw std::invalid_argument("Native one-shot decoder requires bounded PCM WAV");
    const size_t end=size_t(read32(data+4))+8;uint16_t channels=0,bits=0;uint32_t rate=0;
    const unsigned char *pcm=nullptr;size_t pcmBytes=0;
    for(size_t at=12;at+8<=end;) {
        const size_t bytes=read32(data+at+4),payload=at+8;if(bytes>end-payload)throw std::invalid_argument("Truncated WAV chunk");
        if(!std::memcmp(data+at,"fmt ",4)){
            if(bytes<16 || read16(data+payload)!=1)throw std::invalid_argument("Unsupported WAV codec");
            channels=read16(data+payload+2);rate=read32(data+payload+4);bits=read16(data+payload+14);
        }else if(!std::memcmp(data+at,"data",4)){pcm=data+payload;pcmBytes=bytes;}
        const auto padded=bytes+(bytes&1);if(padded>end-payload){if(bytes==end-payload)break;throw std::invalid_argument("Truncated WAV padding");}at=payload+padded;
    }
    if(!pcm || !pcmBytes || (channels!=1&&channels!=2) || (bits!=8&&bits!=16) || rate<8000 || rate>192000 ||
       pcmBytes%(channels*(bits/8)) || pcmBytes/(bits/8)>MaxBytes/2)throw std::invalid_argument("Invalid PCM WAV data");
    const auto samples=pcmBytes/(bits/8);auto *output=static_cast<int16_t *>(std::malloc(samples*2));if(!output)throw std::bad_alloc();
    for(size_t i=0;i<samples;++i)output[i]=bits==8?int16_t((int(pcm[i])-128)*256):int16_t(read16(pcm+i*2));
    return {static_cast<unsigned int>(samples/channels),rate,16,channels,output};
}
void UnloadWave(Wave wave){std::free(wave.data);}
