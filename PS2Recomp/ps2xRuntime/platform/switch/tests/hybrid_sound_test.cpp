#include "host_declarations.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
void mixSwitchSounds(int16_t *,size_t);
int main()
{
    unsigned checks=0;
    auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("native one-shot sound regression");};
    const std::array<int16_t,4> pcm={1000,-1000,2000,-2000};
    Wave wave{2,48000,16,2,const_cast<int16_t *>(pcm.data())};
    auto sound=LoadSoundFromWave(wave);check(!IsSoundPlaying(sound));PlaySound(sound);check(IsSoundPlaying(sound));
    std::array<int16_t,6> output{};mixSwitchSounds(output.data(),3);
    check(output[0]==1000 && output[1]==-1000 && output[2]==2000 && output[3]==-2000 && output[4]==0);
    check(!IsSoundPlaying(sound));
    SetSoundVolume(sound,0.5f);PlaySound(sound);output.fill(0);mixSwitchSounds(output.data(),1);check(output[0]==500 && output[1]==-500);
    StopSound(sound);check(!IsSoundPlaying(sound));UnloadSound(sound);
    bool rejected=false;try{PlaySound(sound);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    rejected=false;try{(void)LoadWaveFromMemory(".ogg",nullptr,0);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    wave.sampleRate=24000;wave.channels=1;wave.frameCount=4;
    sound=LoadSoundFromWave(wave);PlaySound(sound);output.fill(0);mixSwitchSounds(output.data(),3);
    check(output[0]==1000 && output[1]==1000 && output[2]==0 && output[4]==-1000);
    UnloadSound(sound);
    std::array<unsigned char,48> wav{};
    auto word=[&](unsigned at,uint32_t value){for(unsigned i=0;i<4;++i)wav[at+i]=uint8_t(value>>(i*8));};
    std::memcpy(wav.data(),"RIFF",4);word(4,40);std::memcpy(wav.data()+8,"WAVEfmt ",8);word(16,16);
    wav[20]=1;wav[22]=1;word(24,48000);word(28,48000);wav[32]=1;wav[34]=8;
    std::memcpy(wav.data()+36,"data",4);word(40,4);wav[44]=128;wav[45]=255;wav[46]=0;wav[47]=128;
    auto decoded=LoadWaveFromMemory(".wav",wav.data(),wav.size());
    check(decoded.frameCount==4&&decoded.sampleRate==48000&&decoded.sampleSize==16&&decoded.channels==1);
    const auto *samples=static_cast<int16_t *>(decoded.data);check(samples[0]==0&&samples[1]==32512&&samples[2]==-32768);UnloadWave(decoded);
    wav[34]=16;wav[32]=2;word(28,96000);wav[44]=0x34;wav[45]=0x12;wav[46]=0xFE;wav[47]=0xFF;
    decoded=LoadWaveFromMemory(".wav",wav.data(),wav.size());samples=static_cast<int16_t *>(decoded.data);
    check(decoded.frameCount==2&&samples[0]==0x1234&&samples[1]==-2);UnloadWave(decoded);
    const auto good=wav;
    for(const auto offset:{0u,8u,20u,22u,34u}){wav=good;wav[offset]=0xFF;rejected=false;
        try{auto invalid=LoadWaveFromMemory(".wav",wav.data(),wav.size());UnloadWave(invalid);}catch(const std::invalid_argument &){rejected=true;}check(rejected);}
    wav=good;word(40,100);rejected=false;try{(void)LoadWaveFromMemory(".wav",wav.data(),wav.size());}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    wave.sampleSize=8;rejected=false;try{(void)LoadSoundFromWave(wave);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    std::printf("hybrid native sound: %u checks passed\n",checks);
}
