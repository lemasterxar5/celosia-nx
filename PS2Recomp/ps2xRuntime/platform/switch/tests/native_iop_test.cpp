#include "native_iop.hpp"
#include "native_memory.hpp"
#include "native_heap.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    using namespace celosia::native;
    DataMemory data;
    DataHeap heap(data);
    require(heap.configure(0x10000,0x20000),"test native heap configured");
    NativeServices native(data,heap);
    require(!native.ready(),"services opt-in");
    bool rejected=false;
    try { (void)native.services(); } catch(const std::logic_error &) { rejected=true; }
    require(rejected,"uninitialized services fail explicitly");
    int16_t pcm[2]{17,19};
    require(native.readAudio(pcm,1)==0 && pcm[0]==17,"inactive producer reports no PCM");
    auto &host=native.host();
    uint32_t normalized=99;
    require(host.normalizeGuestAddress(0x80000200,normalized) && normalized==0x200,"native data alias normalization");
    require(!host.normalizeGuestAddress(0x12000000,normalized),"device address cannot enter native host");
    require(!host.readIopMemory(0,nullptr,0) && !host.writeIopMemory(0,nullptr,0) &&
            !host.zeroIopMemory(0,0) && !host.normalizeIopAddress(0,normalized),"inactive arena access fails");
    uint64_t fileSize=9; size_t bytesRead=99;
    require(host.openHostFile("unused")==0 && !host.hostFileSize(1,fileSize) &&
            !host.readHostFile(1,0,nullptr,0,bytesRead) && bytesRead==0,"missing file providers cannot fake data");
    require(host.hostPath(ps2x::iop::HostPathKind::CdRoot).empty(),"missing root has no fabricated path");
    require(host.memoryCard({})==-1 && !host.hasGuestFunction(0x100010),"missing save/callback providers fail");
    auto rejects=[&](auto action) { bool caught=false; try { action(); } catch(const std::exception &) { caught=true; } require(caught,"unsupported host operation fails explicitly"); };
    rejects([&]{(void)host.translateGuestPath("unused");});
    rejects([&]{host.closeHostFile(1);});
    rejects([&]{host.audioCommand(0,0,{},{});});
    rejects([&]{(void)host.invokeGuestFunction(0,0,0,0,0,0,nullptr);});
    rejects([&]{(void)host.sendSifCommand(0,nullptr,0);});
    host.closeHostFile(0);
    const auto allocation=host.allocateGuest(32,16);
    require(allocation && host.allocateIopHandle(ps2x::iop::IopHandleKind::RpcServer)!=
            host.allocateIopHandle(ps2x::iop::IopHandleKind::RpcServer),"native heap and service identities allocated");
    const std::array<uint8_t,4> value{4,3,2,1};
    std::array<uint8_t,4> copy{};
    require(host.writeGuest(allocation,value.data(),4) && host.readGuest(allocation,copy.data(),4) && copy==value &&
            host.zeroGuest(allocation,4),"host accesses bounded game data");
    host.freeGuest(allocation); host.freeGuest(0);
    rejects([&]{host.freeGuest(allocation);});
    native.initialize();
    require(native.ready(),"native service owner initialized");
    auto &services=native.services();
    native.initialize();
    require(&services==&native.services(),"repeat init preserves service and arena ownership");
    const auto block=services.allocateMemory(32);
    require(block>0 && services.isMemoryRange(block,32),"real native audio arena allocation");
    const std::array<uint8_t,4> payload{1,2,3,4};
    std::array<uint8_t,4> copied{};
    require(services.writeMemory(block,payload.data(),4) && services.readMemory(block,copied.data(),4) && copied==payload,
            "native arena bytes preserved");
    require(host.normalizeIopAddress(block,normalized) && normalized==block &&
            host.writeIopMemory(block,payload.data(),4) && host.readIopMemory(block,copied.data(),4) &&
            host.zeroIopMemory(block,4),"native host arena access is bounded storage");
    require(services.freeMemory(block) && !services.freeMemory(block),"arena ownership checked");
    require(!services.canBindRpc(0x11223344),"sound route inactive before native module selection");
    require(services.loadModule("rom0:SIO2MAN").moduleId<0,
            "controller module cannot load without a native controller provider");
    require(services.loadModule("rom0:SIO2D").moduleId<0 && services.loadModule("DS1O_D.IRX").moduleId<0,
            "all controller aliases require the actual native provider");
    require(services.loadModule("USBD.IRX").moduleId<0 && services.loadModule("PL2303.IRX").moduleId<0,
            "unimplemented USB/debug modules cannot become fake native libraries");
    const auto module=services.loadModule("cdrom0:\\SG2IOPM1.IRX;1");
    require(module.handled && module.moduleId>0 && services.canBindRpc(0x11223344),"native sound selected without opening/executing IRX");
    require(module.moduleId==0x40000000,"failed controller/USB activation does not consume module identities");
    require(services.loadModule("cdrom0:\\UNKNOWN.IRX;1").moduleId<0,"unknown module cannot fake success");
    ps2x::iop::RpcRequest request; request.sid=0xFFFFFFFF;
    require(!services.handleRpc(request).handled,"unknown RPC cannot fake completion");
    constexpr uint32_t dbcSid=0x80001300,dbcVersion=0x80001363,dbcOutput=0x22000;
    require(!services.canBindRpc(dbcSid) && services.loadModule("DBCMAN.IRX").moduleId>0 &&
            services.canBindRpc(dbcSid),"DBC version route requires an actual loaded native service");
    ps2x::iop::RpcRequest dbc; dbc.sid=dbcSid; dbc.function=0xFFFFFFFF;
    dbc.receive={dbcOutput,16};
    const std::array<uint32_t,5> canary{0xDEADBEEF,0xDEADBEEF,0xDEADBEEF,0xDEADBEEF,0xDEADBEEF};
    std::array<uint32_t,5> dbcWords{};
    require(data.write(dbcOutput,canary.data(),sizeof(canary)),"DBC output fixture stored");
    require(!services.handleRpc(dbc).handled && data.read(dbcOutput,dbcWords.data(),sizeof(dbcWords)) &&
            dbcWords==canary,"unknown DBC command cannot claim completion or alter output");
    dbc.function=dbcVersion;
    for(const auto invalid:std::array<ps2x::iop::GuestBuffer,4>{{{0,16},{dbcOutput,0},
            {dbcOutput,3},{DataMemory::MainBytes-2,16}}}) {
        dbc.receive=invalid;
        const auto result=services.handleRpc(dbc);
        require(!result.handled && result.resultAddress==0,"missing, short or invalid DBC output fails explicitly");
    }
    dbc.receive={dbcOutput,16};
    const auto versionReply=services.handleRpc(dbc);
    require(versionReply.handled && versionReply.resultAddress==dbcOutput &&
            data.read(dbcOutput,dbcWords.data(),sizeof(dbcWords)) &&
            dbcWords==std::array<uint32_t,5>{0x310,0x310,0x310,0x310,0xDEADBEEF},
            "DBC supported version writes actual bounded reply without touching trailing data");
    dbc.receive={0x80000000u+dbcOutput,4};
    require(services.handleRpc(dbc).handled,"DBC bounded one-word version reply accepts native data alias");
    require(services.stopModule(services.loadModule("DBCMAN.IRX").moduleId),
            "DBC module reference can be released");
    const auto stale=services.allocateMemory(64);
    native.reset();
    require(!services.canBindRpc(0x11223344) && !services.freeMemory(stale),"native reset clears real service routes and owned audio allocations");
    rejected=false;
    try { native.configureFiles({}); } catch(const std::logic_error &) { rejected=true; }
    require(rejected,"configuration frozen after initialization");
    NativeServices configured(data,heap);
    NativeFileAccess files;
    bool closed=false,logged=false;
    files.rootPath=[](auto){return std::string("native-assets");};
    files.translate=[](auto){return std::string("native-assets/TEST.BIN");};
    files.open=[](auto){return uint64_t(11);};
    files.size=[](uint64_t,uint64_t &size){size=4;return true;};
    files.read=[&](uint64_t,uint64_t,std::span<uint8_t> out,size_t &read){
        read=out.size(); for(auto &byte:out) byte=7; return true;
    };
    files.close=[&](auto){closed=true;};
    files.memoryCard=[](auto &){return int32_t(-2);};
    files.log=[&](auto,auto){logged=true;};
    configured.configureFiles(std::move(files));
    auto &bound=configured.host();
    require(bound.hostPath(ps2x::iop::HostPathKind::CdRoot)=="native-assets" &&
            bound.translateGuestPath("TEST.BIN")=="native-assets/TEST.BIN" && bound.openHostFile("TEST.BIN")==11,
            "explicit native file callbacks route paths and handles");
    require(bound.hostFileSize(11,fileSize) && fileSize==4 &&
            bound.readHostFile(11,0,copied.data(),4,bytesRead) && bytesRead==4 && copied[0]==7,
            "native file data callback used");
    require(!bound.readHostFile(11,0,nullptr,1,bytesRead),"null output rejected before file callback");
    bound.closeHostFile(11); bound.log(ps2x::iop::LogLevel::Info,"test");
    require(closed && logged && bound.memoryCard({})==-2,"close/log/save errors propagate from callbacks");
    unsigned activations=0,resets=0;
    bool available=false,throwActivation=false;
    configured.configureControllerHooks([&] {
        ++activations;
        if(throwActivation) throw std::runtime_error("injected native controller initialization error");
        return available;
    },[&]{ ++resets; });
    configured.initialize();
    auto &selected=configured.services();
    require(selected.loadModule("rom0:SIO2MAN").moduleId<0 && activations==1 && resets==0,
            "failed native initialization cannot publish a module or active state");
    throwActivation=true;
    require(selected.loadModule("SIO2D.IRX").moduleId<0 && activations==2 && resets==0,
            "native initialization exception is an explicit module failure");
    throwActivation=false; available=true;
    const auto selectedSound=selected.loadModule("SG2IOPM1.IRX");
    const auto manager=selected.loadModule("rom0:SIO2MAN");
    const auto transport=selected.loadModule("SIO2D.IRX");
    const auto pad=selected.loadModule("cdrom0:\\MODULES\\DS1O_D.IRX;1");
    require(selectedSound.moduleId==0x40000000 && manager.moduleId==0x40000001 &&
            transport.moduleId>manager.moduleId && pad.moduleId>transport.moduleId,
            "native controller aliases use the shared service module identity namespace");
    require(selected.loadModule("SIO2MAN.IRX").moduleId==manager.moduleId && activations==3,
            "aliases and repeated references share one actual native activation");
    bool sawNativeController=false;
    for(const auto &service:selected.debugSnapshot().services) if(service.name=="native-controller")
        sawNativeController=service.active && service.sids.empty();
    require(sawNativeController,"native input library has active diagnostics without invented RPC SIDs");
    require(selected.stopModule(manager.moduleId) && selected.stopModule(transport.moduleId) && resets==0,
            "live alias/reference keeps native controller active");
    require(selected.stopModule(manager.moduleId) && resets==0 && selected.stopModule(pad.moduleId) && resets==1,
            "last controller alias/reference resets actual native provider exactly once");
    require(selected.canBindRpc(0x11223344),"controller unload preserves loaded native sound service");
    require(selected.loadModule("DS1O_D.IRX").moduleId>pad.moduleId && activations==4,
            "controller can activate again after complete unload");
    configured.reset();
    require(resets==2 && !selected.canBindRpc(0x11223344),"full reset closes active controller and sound routes");
    configured.reset();
    require(resets==2,"inactive full reset does not repeat native provider cleanup");
    rejected=false;
    try { configured.configureControllerHooks({},{}); } catch(const std::logic_error &) { rejected=true; }
    require(rejected,"controller callbacks are frozen after service initialization");
    {
        NativeServices scoped(data,heap);
        scoped.configureControllerHooks([]{return true;},[&]{++resets;});
        scoped.initialize();
        require(scoped.services().loadModule("SIO2MAN.IRX").moduleId>0,"scoped controller activation");
    }
    require(resets==3,"service destruction cleans up active native controller provider");
    std::printf("native service host: %u checks passed\n",checks);
}
