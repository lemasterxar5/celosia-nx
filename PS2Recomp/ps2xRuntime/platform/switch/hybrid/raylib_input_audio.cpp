#include "raylib.h"
#include "native_audio_output.hpp"
#include <switch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
void mixSwitchSounds(int16_t *,size_t);
void clearSwitchSounds();
void celosiaMenuInput(uint64_t &buttons,std::array<float,4> &sticks);

namespace {
struct InputState {
    std::mutex mutex;
    PadState pad{};
    bool initialized=false,connected=false;
    uint64_t buttons=0;
    std::array<float,4> sticks{};
} input;
constexpr std::array<uint64_t,18> ButtonBits={
    0,HidNpadButton_Up,HidNpadButton_Right,HidNpadButton_Down,HidNpadButton_Left,
    HidNpadButton_X,HidNpadButton_A,HidNpadButton_B,HidNpadButton_Y,
    HidNpadButton_L,HidNpadButton_ZL,HidNpadButton_R,HidNpadButton_ZR,
    HidNpadButton_Minus,0,HidNpadButton_Plus,HidNpadButton_StickL,HidNpadButton_StickR};
static_assert(GAMEPAD_BUTTON_RIGHT_THUMB==17 && GAMEPAD_AXIS_RIGHT_Y==3);
float axis(int32_t value,bool invert)
{
    const auto wide=invert?-int64_t(value):int64_t(value);
    return float(std::clamp<int64_t>(wide,-32768,32767))/32768.0f;
}
struct AudioState {
    Handle owner=INVALID_HANDLE;
    uintptr_t nextToken=1,token=0;
    int bufferHint=1024;
    std::atomic<AudioCallback> callback{nullptr};
    std::atomic<bool> playing{false};
    // Destroy output/join its callback before the callback state above.
    std::unique_ptr<celosia::native::NativeAudioOutput> output;
} audio;
void audioOwner()
{
    if(audio.output && audio.owner!=threadGetCurHandle())
        throw std::logic_error("Switch audio control must remain on its initializing thread");
}
void operationalAudio()
{
    audioOwner();
    if(!audio.output)throw std::logic_error("Switch audio device is not initialized");
    const auto stats=audio.output->stats();
    if(!stats.running || stats.failure!=celosia::native::AudioFailure::None)
        throw std::runtime_error("Switch audio output is not operational");
}
void validateStream(AudioStream stream)
{
    operationalAudio();
    if(!audio.token || stream.buffer!=reinterpret_cast<void *>(audio.token) || stream.processor!=&audio ||
       stream.sampleRate!=48000 || stream.sampleSize!=16 || stream.channels!=2)
        throw std::invalid_argument("Invalid Switch audio stream");
}
}

// Called once per host frame by PollInputEvents. No second logical controller
// is fabricated from the same physical player. Queries read this frame snapshot.
void pollSwitchInput()
{
    std::lock_guard lock(input.mutex);
    if(!input.initialized) {
        padConfigureInput(1,HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&input.pad);input.initialized=true;
    }
    padUpdate(&input.pad);input.connected=padIsConnected(&input.pad);
    input.buttons=input.connected?padGetButtons(&input.pad):0;
    input.sticks.fill(0);
    if(!input.connected)return;
    const auto left=padGetStickPos(&input.pad,0),right=padGetStickPos(&input.pad,1);
    input.sticks={axis(left.x,false),axis(left.y,true),axis(right.x,false),axis(right.y,true)};
    // The in-game menu (Minus + Plus): while open the game sees no input.
    celosiaMenuInput(input.buttons,input.sticks);
}
bool IsKeyDown(int){return false;}
bool IsKeyPressed(int){return false;}
bool IsGamepadAvailable(int gamepad){std::lock_guard lock(input.mutex);return gamepad==0 && input.connected;}
bool IsGamepadButtonDown(int gamepad,int button)
{
    std::lock_guard lock(input.mutex);
    return gamepad==0 && input.connected && button>0 && size_t(button)<ButtonBits.size() &&
           (input.buttons&ButtonBits[size_t(button)])!=0;
}
float GetGamepadAxisMovement(int gamepad,int which)
{
    std::lock_guard lock(input.mutex);
    if(gamepad!=0 || !input.connected)return 0;
    if(which>=GAMEPAD_AXIS_LEFT_X && which<=GAMEPAD_AXIS_RIGHT_Y)return input.sticks[size_t(which)];
    if(which==GAMEPAD_AXIS_LEFT_TRIGGER)return (input.buttons&HidNpadButton_ZL)?1.0f:-1.0f;
    if(which==GAMEPAD_AXIS_RIGHT_TRIGGER)return (input.buttons&HidNpadButton_ZR)?1.0f:-1.0f;
    return 0;
}
void InitAudioDevice()
{
    if(audio.output){operationalAudio();return;}
    audio.playing.store(false,std::memory_order_release);
    audio.callback.store(nullptr,std::memory_order_release);
    auto output=std::make_unique<celosia::native::NativeAudioOutput>();
    if(!output->start([](int16_t *samples,size_t frames) {
        if(audio.playing.load(std::memory_order_acquire)) {
            const auto callback=audio.callback.load(std::memory_order_acquire);
            if(!callback || frames>std::numeric_limits<unsigned int>::max())
                throw std::runtime_error("Switch audio producer is unavailable");
            callback(samples,static_cast<unsigned int>(frames));
        } else std::fill_n(samples,frames*2,int16_t(0));
        mixSwitchSounds(samples,frames);
        return frames;
    }))throw std::runtime_error("Could not start native Switch audio output");
    audio.owner=threadGetCurHandle();audio.output=std::move(output);
}
void CloseAudioDevice()
{
    audioOwner();
    if(!audio.output)return;
    audio.playing.store(false,std::memory_order_release);
    // The runtime must close this device before destroying its sound producer.
    // stop joins the actual libnx output thread before its callback is released.
    audio.output->stop();audio.output.reset();
    clearSwitchSounds();
    audio.callback.store(nullptr,std::memory_order_release);audio.token=0;audio.owner=INVALID_HANDLE;
}
bool IsAudioDeviceReady()
{
    audioOwner();if(!audio.output)return false;
    const auto status=audio.output->stats();
    return status.running && status.failure==celosia::native::AudioFailure::None;
}
void SetAudioStreamBufferSizeDefault(int size)
{
    audioOwner();
    if(size<256 || size>8192 || size%64)throw std::invalid_argument("Invalid Switch audio buffer hint");
    // This API supplies a host hint. The native output owns three fixed 1024-
    // frame hardware buffers; callbacks receive their actual frame count.
    audio.bufferHint=size;
}
AudioStream LoadAudioStream(unsigned int sampleRate,unsigned int sampleSize,unsigned int channels)
{
    operationalAudio();
    if(sampleRate!=48000 || sampleSize!=16 || channels!=2)
        throw std::invalid_argument("Switch native audio requires 48kHz signed16 stereo");
    if(audio.token)throw std::logic_error("Only one Switch audio stream is supported");
    if(audio.nextToken==std::numeric_limits<uintptr_t>::max())throw std::overflow_error("Switch audio stream identifiers exhausted");
    audio.token=audio.nextToken++;
    return {reinterpret_cast<void *>(audio.token),&audio,48000,16,2};
}
void SetAudioStreamCallback(AudioStream stream,AudioCallback callback)
{
    validateStream(stream);
    if(!callback)throw std::invalid_argument("Switch audio callback is missing");
    if(audio.playing.load(std::memory_order_acquire) && audio.callback.load(std::memory_order_acquire)!=callback)
        throw std::logic_error("Close Switch audio before replacing a live producer");
    audio.callback.store(callback,std::memory_order_release);
}
void PlayAudioStream(AudioStream stream)
{
    validateStream(stream);
    if(!audio.callback.load(std::memory_order_acquire))throw std::logic_error("Switch audio stream has no producer");
    audio.playing.store(true,std::memory_order_release);
}
