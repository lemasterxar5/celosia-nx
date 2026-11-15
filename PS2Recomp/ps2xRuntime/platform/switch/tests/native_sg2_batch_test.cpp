#include "native_iop.hpp"
#include "native_memory.hpp"
#include "native_heap.hpp"
#include "native/audio_arena.h"
#include "native/audio_ram.h"
#include "emulator/services/iop_sg2.h"
#include "ps2x/iop/audio_stats.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

static unsigned checks=0;
static void require(bool ok,const char *why) {++checks;if(!ok) throw std::runtime_error(why);}
using Command=std::array<uint32_t,4>;
class NoTransportHost final : public ps2x::iop::IopHost {
public:
    bool readGuest(uint32_t,void *,size_t) const override {throw std::logic_error("direct batch attempted game transport read");}
    bool writeGuest(uint32_t,const void *,size_t) override {throw std::logic_error("direct batch attempted game transport write");}
    bool zeroGuest(uint32_t,size_t) override {throw std::logic_error("unexpected game data clear");}
    bool normalizeGuestAddress(uint32_t,uint32_t &) const override {return false;}
    uint32_t allocateIopHandle(ps2x::iop::IopHandleKind) override {throw std::logic_error("unexpected transport handle");}
    uint32_t allocateGuest(uint32_t,uint32_t) override {throw std::logic_error("unexpected game allocation");}
    void freeGuest(uint32_t) override {throw std::logic_error("unexpected game free");}
    void audioCommand(uint32_t,uint32_t,ps2x::iop::GuestBuffer,ps2x::iop::GuestBuffer) override {throw std::logic_error("unexpected audio transport");}
    std::string hostPath(ps2x::iop::HostPathKind) const override {return {};}
    std::string translateGuestPath(std::string_view) const override {return {};}
    uint64_t openHostFile(std::string_view) override {return 0;}
    bool hostFileSize(uint64_t,uint64_t &) const override {return false;}
    bool readHostFile(uint64_t,uint64_t,void *,size_t,size_t &read) override {read=0;return false;}
    void closeHostFile(uint64_t) override {throw std::logic_error("unexpected file close");}
    int32_t memoryCard(const ps2x::iop::MemoryCardRequest &) override {return -1;}
    bool hasGuestFunction(uint32_t) const override {return false;}
    bool invokeGuestFunction(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t *) override {throw std::logic_error("unexpected game callback");}
    void log(ps2x::iop::LogLevel,std::string_view) override {}
};
class FailingArena final : public ps2x::iop::detail::AudioRam {
public:
    bool readRam(uint32_t,void *,size_t) const override {return false;}
    bool writeRam(uint32_t,const void *,size_t) override {return false;}
    bool ownsRamRange(uint32_t address,size_t bytes) const override {return address<=0x200000 && bytes<=0x200000-address;}
};
int main()
{
    using namespace celosia::native;
    using namespace ps2x::iop;
    DataMemory memory;DataHeap heap(memory);NativeServices native(memory,heap);
    std::array<uint8_t,400> reply;reply.fill(0xA5);
    const auto untouched=reply;
    require(!native.soundReady() && !native.submitSoundCommands({},reply) && reply==untouched,"uninitialized native sound rejects with reply unchanged");
    native.initialize();
    require(!native.soundReady() && !native.submitSoundCommands({},reply) && reply==untouched,"unloaded native provider cannot pretend to submit");
    const auto module=native.services().loadModule("cdrom0:\\SG2IOPM1.IRX;1");
    require(module.moduleId>0 && native.soundReady(),"actual native SG2 provider loaded");
    require(native.submitSoundCommands({},reply) && std::all_of(reply.begin(),reply.end(),[](auto b){return b==0;}),"empty direct batch returns actual initialized 400-byte status");
    const auto source=native.services().allocateMemory(64),destination=native.services().allocateMemory(64);
    require(source && destination,"real native sound arenas allocated");
    std::array<uint8_t,64> bytes{};for(unsigned i=0;i<bytes.size();++i)bytes[i]=uint8_t(i+1);
    require(native.services().writeMemory(source,bytes.data(),bytes.size()),"native sample source staged");
    const std::array<Command,2> transfer{{{0x13,source,0x10000,64},{0x14,destination,0x10000,64}}};
    require(native.submitSoundCommands(transfer,reply),"direct native sample write/readback batch succeeds");
    std::array<uint8_t,64> copied{};
    require(native.services().readMemory(destination,copied.data(),copied.size()) && copied==bytes,"actual mixer sample memory roundtrip preserves bytes");
    uint32_t busy=1;std::memcpy(&busy,reply.data()+0x184,4);require(busy==0,"completed actual native transfer reports idle");
    const auto keyOns=audio_stats::keyOns.load();
    const std::array<Command,2> invalid{{{7,0,1,0},{UINT32_MAX,0,0,0}}};
    reply=untouched;
    require(!native.submitSoundCommands(invalid,reply) && reply==untouched && audio_stats::keyOns.load()==keyOns,"whole batch prevalidated before key-on effects or reply mutation");
    std::vector<Command> tooMany(513);
    require(!native.submitSoundCommands(tooMany,reply) && reply==untouched,"oversized batch rejected without output mutation");
    tooMany.resize(512);require(native.submitSoundCommands(tooMany,reply),"maximum bounded batch accepted");
    const auto pcm=native.services().allocateMemory(0x1000);require(pcm!=0,"native PCM source allocation");
    std::array<int16_t,0x800> pcmData;pcmData.fill(1000);
    require(native.services().writeMemory(pcm,pcmData.data(),sizeof(pcmData)),"actual nonzero PCM data staged");
    const std::array<Command,5> pcmStart{{{0x11,0,0,0},{6,0,0x3FFF,0x3FFF},{6,1,0x3FFF,0x3FFF},
                                      {0x22,0x7FFF,0x7FFF,0},{0x20,pcm,0x1000,0}}};
    require(native.submitSoundCommands(pcmStart,reply),"native PCM initialization uses actual mixer commands");
    // Startup queues 512 stereo frames and refills at 256 remaining. Read into the
    // next block of a larger ring; a 0x800 ring would wrap back to zero.
    const auto blocksBefore=audio_stats::pcmBlocks.load();
    std::array<int16_t,768> audio{};
    require(native.readAudio(audio.data(),384)==384 && std::any_of(audio.begin(),audio.end(),[](auto s){return s!=0;}),"real 48k stereo mixer produces nonzero source PCM samples");
    uint32_t before=0,after=0;std::memcpy(&before,reply.data()+0x180,4);
    require(native.submitSoundCommands({},reply),"empty batch still refreshes real mixer/stream status");
    std::memcpy(&after,reply.data()+0x180,4);require(before==0 && after==0x800 && audio_stats::pcmBlocks.load()>blocksBefore,"empty batch publishes actual first refill block offset after native sample consumption");
    const auto duplicate=native.services().loadModule("cdrom0:\\SG2IOPM1.IRX;1");
    require(duplicate.moduleId==module.moduleId && native.soundReady(),"repeat load retains the actual module identity");
    require(native.submitSoundCommands({},reply) && reply[0x181]==8,"repeat load preserves live PCM position instead of resetting mixer");
    audio.fill(0);
    require(native.readAudio(audio.data(),384)==384 && std::any_of(audio.begin(),audio.end(),[](auto s){return s!=0;}),"repeat load preserves nonzero live PCM playback");
    require(native.services().stopModule(module.moduleId) && native.soundReady(),"first stop drops only duplicate module reference");
    require(native.submitSoundCommands({},reply),"remaining native module reference accepts real sound status");
    require(native.services().stopModule(module.moduleId) && !native.soundReady(),"last stop unloads native sound route");
    reply=untouched;require(!native.submitSoundCommands({},reply) && reply==untouched,"stopped sound cannot report completion");

    NoTransportHost host;ps2x::iop::detail::AudioArena arena;
    ps2x::iop::detail::Sg2Driver direct(host,arena);direct.reset();
    require(direct.submitCommandBatch({},reply),"driver direct method performs no game/RPC transport access");
    FailingArena broken;ps2x::iop::detail::Sg2Driver failed(host,broken);failed.reset();
    reply=untouched;
    const std::array<Command,1> fail{{{0x20,0x80000,0x800,0}}};
    require(!failed.submitCommandBatch(fail,reply) && reply==untouched,"operational native sample failure never writes successful reply");
    std::array<int16_t,128> silent{};silent.fill(99);
    require(failed.mixer().readOutput(silent.data(),64)==64 && std::all_of(silent.begin(),silent.end(),[](auto s){return s==0;}),"failed PCM start cannot leak uninitialized samples");
    std::printf("native SG2 batch: %u checks passed\n",checks);
}
