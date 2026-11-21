// Full registered AOT boot diagnostic, NOT A PLAYABLE GAME.
// Private ELF bytes are copied as data; only already compiled ARM functions run.
#include "native_aot_runtime.hpp"
#include "native_platform_init.hpp"
#include "native_kernel_publication.hpp"
#include "native_module_image.hpp"
#include "native_game_publication.hpp"
#include "native_boot_services.hpp"
#include "native_elf_data.hpp"
#include "native_display.hpp"
#include "native_audio_output.hpp"
#include "native_files.hpp"
#include "native_scus_assets.hpp"
#include "native_faults.hpp"
#include "native_lifecycle.hpp"
#include "native_backend.h"
#include <switch.h>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <vector>

namespace {
struct RomfsLease { bool ready=false; ~RomfsLease() { if(ready) romfsExit(); } };
constexpr const char *Directory="sdmc:/switch/celosia_nx";
constexpr const char *LogPath="sdmc:/switch/celosia_nx/boot.log";
constexpr std::array<uint8_t,32> ScusHash{
    0xD1,0x6A,0xD7,0x6B,0xA8,0xCD,0xF8,0x76,0x95,0xD9,0x4B,0x13,0x16,0xD5,0x1C,0x1F,
    0x24,0x89,0x59,0xEC,0x07,0x49,0x81,0x4C,0x31,0xBB,0x7A,0x2D,0x55,0xD9,0xA2,0xE1};

std::array<uint8_t,32> nativeDigest(std::span<const uint8_t> data)
{
    std::array<uint8_t,32> digest{};
    sha256CalculateHash(digest.data(),data.data(),data.size());
    return digest;
}

std::vector<uint8_t> readData(celosia::native::NativeFiles &files)
{
    const auto token=files.open("SCUS_974.72");
    if(!token) throw std::runtime_error("Missing private SCUS ELF in native assets directory");
    struct CloseToken { celosia::native::NativeFiles &files; uint64_t token; ~CloseToken(){(void)files.close(token);} } close{files,token};
    const auto size=files.size(token);
    if(!size || *size<52 || *size>celosia::native::ElfDataMaxBytes)
        throw std::runtime_error("Invalid private ELF data size");
    std::vector<uint8_t> bytes(static_cast<size_t>(*size));
    if(files.read(token,0,bytes)!=bytes.size() || files.size(token)!=size)
        throw std::runtime_error("Private ELF data changed or read failed");
    return bytes;
}

void execute(PS2Runtime &runtime,R5900Context &context,FILE *log)
{
    const auto deadline=armGetSystemTick()+armNsToTicks(120000000000ull);
    for(unsigned slice=0;slice<100000 && appletMainLoop();++slice) {
        const auto status=runtime.runSlice(context,65536);
        const auto failure=runtime.services().taskFailure();
        if(!failure.empty()) throw std::runtime_error(failure);
        if(status!=PS2Runtime::SliceResult::Yielded) {
            std::fprintf(log,"AOT returned/stopped status=%u PC=0x%08x; NOT game acceptance\n",
                         unsigned(status),context.pc);
            return;
        }
        if(!(slice%256)) {
            std::fprintf(log,"progress slice=%u PC=0x%08x\n",slice,context.pc);
            std::fflush(log);
        }
        if(armGetSystemTick()>=deadline) throw std::runtime_error("Native boot diagnostic time limit reached");
    }
    throw std::runtime_error("Native boot diagnostic budget/app exit reached");
}
}

int main()
{
    mkdir("sdmc:/switch",0777); mkdir(Directory,0777);
    std::unique_ptr<FILE,decltype(&std::fclose)> log(std::fopen(LogPath,"w"),std::fclose);
    if(!log) return 2;
    std::fprintf(log.get(),"Celosia full AOT native boot diagnostic; NOT THE GAME; build %s %s\n",__DATE__,__TIME__);
    std::fflush(log.get());
    RomfsLease romfs;
    celosia::native::NativeFaults faults;
    celosia::native::NativeLifecycle lifecycle;
    std::unique_ptr<celosia::native::NativeFiles> files;
    std::unique_ptr<celosia::native::NativeDisplay> display;
    std::unique_ptr<PS2Runtime> runtime;
    std::unique_ptr<celosia::native::NativeAudioOutput> audio;
    R5900Context context;
    try {
        if(!std::freopen("sdmc:/switch/celosia_nx/game-stdout.log","w",stdout))
            throw std::runtime_error("Native game stdout log could not open");
        if(R_FAILED(romfsInit())) throw std::runtime_error("Native boot shader ROMFS initialization failed");
        romfs.ready=true;
        display=std::make_unique<celosia::native::NativeDisplay>();
        runtime=std::make_unique<PS2Runtime>();
        mkdir("sdmc:/switch/celosia_nx/assets",0777);
        files=std::make_unique<celosia::native::NativeFiles>("sdmc:/switch/celosia_nx/assets");
        celosia::native::NativeFileAccess access;
        access.rootPath=[](ps2x::iop::HostPathKind kind) {
            return kind==ps2x::iop::HostPathKind::CdImage || kind==ps2x::iop::HostPathKind::MemoryCardRoot ?
                std::string{} : std::string("sdmc:/switch/celosia_nx/assets");
        };
        access.translate=[&](std::string_view path) { return files->translate(path); };
        access.open=[&](std::string_view path) { return files->open(path); };
        access.size=[&](uint64_t token,uint64_t &size) {
            const auto result=files->size(token); if(!result) return false; size=*result; return true;
        };
        access.read=[&](uint64_t token,uint64_t offset,std::span<uint8_t> out,size_t &count) {
            const auto result=files->read(token,offset,out); count=result ? *result : 0; return bool(result);
        };
        access.close=[&](uint64_t token) {
            if(!files->close(token)) throw std::runtime_error("Native asset handle close failed");
        };
        access.log=[&](auto,std::string_view message) {
            std::fputs("Native services: ",log.get());
            (void)std::fwrite(message.data(),1,message.size(),log.get());
            std::fputc('\n',log.get()); std::fflush(log.get());
        };
        runtime->services().services().configureFiles(std::move(access));
        const auto bytes=readData(*files);
        std::array<uint8_t,32> digest{};
        sha256CalculateHash(digest.data(),bytes.data(),bytes.size());
        if(digest!=ScusHash) throw std::runtime_error("Private SCUS data SHA-256 does not match this AOT build");
        const auto image=celosia::native::loadElfData(runtime->memory(),bytes,0x100010);
        if(image.gp!=0x141D70 || image.dataEnd!=0x16A398 || image.loadSegments!=2)
            throw std::runtime_error("Private SCUS ELF metadata does not match native library bindings");
        runtime->services().library().enableScus();
        runtime->services().configureAssets(*files,celosia::native::ScusAssets);
        runtime->services().configureSaveRoot("sdmc:/switch/celosia_nx/saves");
        runtime->services().library().setDisplayInitializer([&] {
            display->initialize();
            std::fprintf(log.get(),"Native deko3d display initialized: 1280x720; empty boot frame, not game rendering\n");
            std::fflush(log.get());
        });
        runtime->services().library().setExceptionInitializer([&] {
            if(!faults.initialize()) throw std::runtime_error("Native ARM fault handler is unavailable");
        });
        runtime->services().library().setPowerOffInitializer([&] {
            if(!lifecycle.prepare()) throw std::runtime_error("Native applet lifecycle observer could not initialize");
        });
        runtime->services().library().setTransferSynchronizer([&] { display->synchronize(); });
        runtime->services().library().setGraphicsInitializer([&] {
            display->initialize();display->synchronize();
            std::fprintf(log.get(),"Native graphics library initialized on deko3d worker; viewport=1280x720\n");
            std::fflush(log.get());
        });
        runtime->services().library().setPlatformInitializer([&] {
            celosia::native::initializeScusPlatform(runtime->memory(),runtime->services(),[&] {
                if(!lifecycle.prepare()) throw std::runtime_error("Native applet lifecycle setup failed");
            },[&] { display->synchronize(); },[&](std::string_view message) {
                const auto bytes=std::fwrite(message.data(),1,message.size(),log.get());
                std::fflush(log.get());
                return bytes==message.size() ? static_cast<int>(bytes) : -1;
            });
        });
        runtime->services().library().setModulePublisher([&](const R5900Context &module) {
            if(getRegU32(&module,17)==0x1B32A0)
                celosia::native::validateKernelPublication(runtime->memory(),module,*files,nativeDigest);
            else celosia::native::validateGamePublication(runtime->memory(),module,*files,nativeDigest);
            std::fprintf(log.get(),"Native module data verified against static AOT input; ARM entry=0x%08x\n",getRegU32(&module,17));
            std::fflush(log.get());
        });
        runtime->services().library().setModuleImageInitializer([&](const R5900Context &module) {
            return celosia::native::loadNativeModuleImage(runtime->memory(),runtime->services(),module,nativeDigest);
        });
        runtime->services().services().initialize();
        audio=std::make_unique<celosia::native::NativeAudioOutput>();
        if(!audio->start([&](int16_t *out,size_t frames) { return runtime->services().services().readAudio(out,frames); }))
            throw std::runtime_error("Native audout producer connection failed");
        std::fprintf(log.get(),"Native audout connected to native sound producer; assets use native readonly files\n");
        std::fprintf(log.get(),"ELF data loaded: entry=0x%08x GP=0x%08x end=0x%08x segments=%u\n",
                     image.entry,image.gp,image.dataEnd,image.loadSegments);
        if(!runtime->guestFunctionAt(image.entry)) throw std::runtime_error("Full AOT registration missing entry");
        std::fprintf(log.get(),"Full static function table slots=%u; no instruction decoder/device runtime\n",
                     g_ps2RecompiledFunctionTableSlotCount);
        std::fflush(log.get());
        context.pc=image.entry;
        execute(*runtime,context,log.get());
        runtime->requestStop();
        std::fprintf(log.get(),"Diagnostic finished; gameplay and rendering remain unverified\n");
        return 0;
    } catch(const std::exception &error) {
        if(runtime) runtime->requestStop();
        std::fprintf(log.get(),"BLOCKED PC=0x%08x SP=0x%08x GP=0x%08x: %s\n",
                     context.pc,getRegU32(&context,29),getRegU32(&context,28),error.what());
        for(unsigned reg=4;reg<12;++reg)
            std::fprintf(log.get(),"arg%u=0x%08x%s",reg-4,getRegU32(&context,reg),reg==11?"\n":" ");
        return 1;
    }
}
