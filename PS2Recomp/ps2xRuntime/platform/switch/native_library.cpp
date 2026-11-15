#include "native_library.hpp"
#include "native_clock.hpp"
#include "native_memory.hpp"
#include "native_semaphore.hpp"
#include "native_aot_context.hpp"
#include "native_threads.hpp"
#include "native_printf.hpp"
#include "native_kernel_services.hpp"
#include "native_math_init.hpp"
#include "native_sg2_control.hpp"
#include <array>
#include <stdexcept>

namespace celosia::native {
struct NativeLibrary::Impl {
    DataMemory &data;
    SemaphoreSet &semaphores;
    NativeThreads &threads;
    SystemClock clock;
    NativeKernelServices kernelServices;
    std::unique_ptr<NativeSg2Control> sound;
    std::function<void()> initializeDisplay;
    std::function<void()> initializeExceptions,initializePowerOff,synchronizeTransfers,initializePlatform,initializeGraphics;
    std::function<void(const R5900Context &)> publishModule;
    std::function<int32_t(const R5900Context &)> initializeModuleImage;
    std::function<bool(uint32_t,R5900Context &)> save;
    std::array<int32_t,2> resources{SemaphoreSet::Error,SemaphoreSet::Error};
    int32_t owner=-1;
    bool enabled=false,ready=false,kernelPublished=false,managerPublished=false,gamePublished=false;
    Impl(DataMemory &memory,SemaphoreSet &sync,NativeThreads &tasks,NativeServices *audio)
        : data(memory),semaphores(sync),threads(tasks),kernelServices(memory,sync)
    {if(audio)sound=std::make_unique<NativeSg2Control>(memory,*audio);}
    void initialize()
    {
        if(ready) return;
        std::array<int32_t,2> ids{SemaphoreSet::Error,SemaphoreSet::Error};
        try {
            for(unsigned i=0;i<2;++i) {
                ids[i]=semaphores.create(1,1,0,0x138250+i*16);
                if(ids[i]<=0) throw std::runtime_error("Native library: semaphore creation failed");
            }
            if(!clock.initialize() || !clock.start()) throw std::logic_error("Native library clock state");
            const auto identity=threads.registerMain();
            if(identity<=0) throw std::runtime_error("Native library: native main thread registration failed");
            // Ordinary SCUS library data only. Kernel syscall-table pointers,
            // TLB patches and interrupt-register state are never fabricated.
            if(!data.write(0x1329A0,ids.data(),sizeof(ids)) || !data.zero(0x133938,8) ||
               !data.zero(0x14AC88,0x200)) throw std::logic_error("Native library data bounds");
            resources=ids; owner=identity; ready=true;
        } catch(...) {
            for(const auto id:ids) if(id>0) (void)semaphores.erase(id);
            (void)clock.end();
            throw;
        }
    }
};
NativeLibrary::NativeLibrary(DataMemory &data,SemaphoreSet &sync,NativeThreads &threads,NativeServices *sound)
    : impl(std::make_unique<Impl>(data,sync,threads,sound)) {}
NativeLibrary::~NativeLibrary()
{ for(const auto id:impl->resources) if(id>0) (void)impl->semaphores.erase(id); }
void NativeLibrary::enableScus() { impl->enabled=true; }
void NativeLibrary::setDisplayInitializer(std::function<void()> initialize)
{ impl->initializeDisplay=std::move(initialize); }
void NativeLibrary::setExceptionInitializer(std::function<void()> initialize)
{ impl->initializeExceptions=std::move(initialize); }
void NativeLibrary::setPowerOffInitializer(std::function<void()> initialize)
{ impl->initializePowerOff=std::move(initialize); }
void NativeLibrary::setTransferSynchronizer(std::function<void()> synchronize)
{ impl->synchronizeTransfers=std::move(synchronize); }
void NativeLibrary::setPlatformInitializer(std::function<void()> initialize)
{ impl->initializePlatform=std::move(initialize); }
void NativeLibrary::setGraphicsInitializer(std::function<void()> initialize)
{ impl->initializeGraphics=std::move(initialize); }
void NativeLibrary::setModuleImageInitializer(std::function<int32_t(const R5900Context &)> initialize)
{ impl->initializeModuleImage=std::move(initialize); }
void NativeLibrary::setSaveHandler(std::function<bool(uint32_t,R5900Context &)> handle)
{ impl->save=std::move(handle); }
void NativeLibrary::setModulePublisher(std::function<void(const R5900Context &)> publish)
{ impl->publishModule=std::move(publish); }
void NativeLibrary::publishModule(const R5900Context &context)
{
    auto &s=*impl;
    const auto entry=getRegU32(&context,17),caller=getRegU32(&context,31);
    const bool kernel=entry==0x1B32A0 && caller==0x101D3C;
    const bool manager=entry==0x1082ED0 && caller==0x1B64D0;
    const bool game=entry==0x1344788 && caller==0x1B64D0;
    if(!s.enabled || !s.ready || !s.publishModule || (!kernel && !manager && !game) ||
       (!kernel && !s.kernelPublished))
        throw std::runtime_error("Native module: unsupported publication boundary");
    if(kernel)s.kernelPublished=s.managerPublished=s.gamePublished=false;
    else if(manager)s.managerPublished=s.gamePublished=false;
    else s.gamePublished=false;
    s.publishModule(context);
    if(kernel) {
        s.kernelServices.reset();
        if(s.sound)s.sound->reset();
        s.kernelPublished=true;
    } else if(manager)s.managerPublished=true;
    else s.gamePublished=true;
}
void NativeLibrary::requirePublishedEntry(uint32_t address) const
{
    if(address>=0x1AC998 && address<0x1E0B3C && !impl->kernelPublished)
        throw std::runtime_error("Native module: unpublished kernel entry");
    if(address>=0x107B2C8 && address<0x116AE80 && !impl->managerPublished)
        throw std::runtime_error("Native module: unpublished manager entry");
    if(address>=0x1339E68 && address<0x14653E8 && !impl->gamePublished)
        throw std::runtime_error("Native module: unpublished gamecore entry");
}
bool NativeLibrary::ready() const noexcept { return impl->ready; }
int32_t NativeLibrary::currentThreadId() const
{ return impl->ready ? impl->threads.currentId() : -1; }
bool NativeLibrary::handle(uint32_t address,R5900Context &context)
{
    auto &s=*impl;
    if(!s.enabled) return false;
    if(address==0x1AD460 || address==0x1AD510 || address==0x1AD528) {
        if(!s.kernelPublished) throw std::runtime_error("Native kernel services require verified module data");
        return s.kernelServices.handle(address,context);
    }
    switch(address) {
    case 0x1CBC48:case 0x1CB8E8: {
        if(!s.kernelPublished)throw std::runtime_error("Native optional input requires verified module data");
        // No native mouse/keyboard provider is configured. Preserve the game's
        // ordinary unavailable state without initializing a physical transport.
        const uint32_t unavailable=0;
        if(!s.data.write(address==0x1CBC48?0x1E1574:0x1E156C,&unavailable,4))
            throw std::logic_error("Native optional input metadata bounds failed");
        setReturnU32(&context,0);break;
    }
    case 0x1B06F8:case 0x1B07F0:case 0x1B0918:case 0x1B09C0:case 0x1B0A68:
    case 0x1B0B10:case 0x1B0BB8:case 0x1B0CB0:case 0x1B0DA8:case 0x1B0DF0:case 0x1B0E38:case 0x1B0F20:
        if(!s.kernelPublished || !s.save)throw std::runtime_error("Native save storage requires verified module and provider");
        if(!s.save(address,context))throw std::logic_error("Native save entry was not handled");
        return true;
    case 0x1D2010:case 0x1D20A0:case 0x1D2150:case 0x1D2170:case 0x1D2188:
    case 0x1D2198:case 0x1D21B0:case 0x1D21C8:case 0x1D21E8:case 0x1D23D8:
        if(!s.kernelPublished || !s.sound) throw std::runtime_error("Native sound requires verified module and provider");
        return s.sound->handle(address,context);
    case 0x102D70:
        if(!s.initializeModuleImage) throw std::runtime_error("Native module image initializer is not configured");
        setReturnS32(&context,s.initializeModuleImage(context));break;
    case 0x1C2B50: {
        if(!s.kernelPublished || !s.initializeGraphics)
            throw std::runtime_error("Native graphics bootstrap requires verified module and backend");
        s.initializeGraphics();
        // InitGp: empty CPU draw group plus the library's active/initial state.
        // The original GS reset/upload paths are replaced by real deko3d work.
        const uint16_t count=0;const std::array<uint32_t,2> state{1,0};
        if(!s.data.write(0x2132C8,&count,sizeof(count)) || !s.data.write(0x2138B0,state.data(),sizeof(state)))
            throw std::logic_error("Native graphics metadata bounds failed");
        setReturnU32(&context,0x213810);break;
    }
    case 0x1B7D18:
        if(!s.kernelPublished) throw std::runtime_error("Native math requires verified module data");
        initializeNativeMath(s.data,s.semaphores);setReturnU32(&context,1);break;
    case 0x10FCC8: case 0x10FF00: case 0x10F3C8:
        throw std::runtime_error("Native library: PS2 kernel patch entry is forbidden");
    case 0x10FDD8:
        if(s.ready && currentThreadId()!=s.owner) throw std::runtime_error("Native library: foreign initialization thread");
        s.initialize(); setReturnU32(&context,0); break;
    case 0x106ED0: setReturnS32(&context,currentThreadId()); break;
    case 0x107768: setReturnS32(&context,currentThreadId()==s.owner ? s.owner : -1); break;
    case 0x105948: setReturnS32(&context,nativePrintf(s.data,context,true)); break;
    case 0x105990: setReturnS32(&context,nativeWriteText(s.data,getRegU32(&context,5))); break;
    case 0x1033B0:
        if(!s.initializePlatform) throw std::runtime_error("Native platform bootstrap is not configured");
        s.initializePlatform(); setReturnU32(&context,0); break;
    case 0x1020C0:
        if(!s.initializeExceptions) throw std::runtime_error("Native exception reporting is not configured");
        s.initializeExceptions(); setReturnU32(&context,0); break;
    case 0x104F60:
        if(!s.initializePowerOff) throw std::runtime_error("Native lifecycle is not configured");
        s.initializePowerOff(); setReturnU32(&context,0); break;
    case 0x114940:
        if(getRegU32(&context,4)!=1 || !s.synchronizeTransfers)
            throw std::runtime_error("Native graphics transfer synchronization is not configured for this operation");
        s.synchronizeTransfers(); setReturnU32(&context,0); break;
    case 0x105CE0: {
        if(!s.initializeDisplay) throw std::runtime_error("Native library: display backend is not configured");
        s.initializeDisplay();
        // Logical dimensions of the SCUS loader's debug text grid, not the
        // native scene/swapchain resolution (which is fixed at 1280x720).
        const std::array<uint32_t,3> debugGrid{640,448,0};
        if(!s.data.write(0x13A130,debugGrid.data(),sizeof(debugGrid)))
            throw std::logic_error("Native library: debug grid bounds");
        setReturnU32(&context,0); break;
    }
    case 0x110430:
        if(getRegU32(&context,4)!=2) throw std::runtime_error("Native library: unsupported timer selection");
        setReturnU32(&context,s.clock.initialize() ? 0 : 0x80008002); break;
    case 0x110588: setReturnU32(&context,s.clock.end() ? 0 : 0x80008001); break;
    case 0x110670: setReturnU32(&context,s.clock.start() ? 0 : 1); break;
    case 0x110700: setReturnU32(&context,s.clock.stop() ? 1 : 0); break;
    case 0x110C78: case 0x110CC8: setReturnU64(&context,s.clock.now()); break;
    default: return false;
    }
    context.pc=getRegU32(&context,31);
    return true;
}
}
