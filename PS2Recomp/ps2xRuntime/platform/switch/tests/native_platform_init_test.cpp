#include "native_platform_init.hpp"
#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_assets.hpp"
#include "native_files.hpp"
#include "native_file_api.hpp"
#include "native_pad_api.hpp"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
uint32_t word(DataMemory &memory,uint32_t at)
{ uint32_t value=0; require(memory.read(at,&value,4),"read source metadata"); return value; }
template<class Action> void rejects(Action action,const char *why)
{ bool caught=false; try {action();} catch(const std::exception &) {caught=true;} require(caught,why); }
}
int main()
{
    const auto fixture=std::filesystem::absolute("native-platform-init-test-fixture");
    require(!std::filesystem::exists(fixture),"exclusive fixture");
    std::filesystem::create_directory(fixture);
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::filesystem::remove_all(path);} } cleanup{fixture};
    { std::ofstream file(fixture/"TEST.BIN",std::ios::binary); file<<"data"; }
    NativeFiles files(fixture.generic_string());
    DataMemory memory; BootServices boot(memory);
    require(boot.heap().configure(0x10000,0x20000),"configured native heap");
    unsigned polls=0,lifecycle=0,sync=0;
    boot.configureInput([&]{++polls; NativePadSample sample; sample.connected=true; return sample;});
    const std::array<AssetEntry,1> entries{{{"TEST.BIN",0,4}}};
    boot.configureAssets(files,entries);
    const auto fd=boot.fileApi().open("TEST.BIN"); require(fd>0,"owned file open before reset");
    std::string log;
    auto sink=[&](std::string_view message){log.append(message);return int(message.size());};
    auto prepare=[&]{++lifecycle;}; auto finish=[&]{++sync;};
    initializeScusPlatform(memory,boot,prepare,finish,sink);
    require(lifecycle==1 && sync==1,"real lifecycle and GPU synchronization callbacks invoked");
    require(boot.assets().ready() && !boot.fileApi().size(fd),"actual asset initialization and descriptor reset");
    const auto first=static_cast<int32_t>(word(memory,0x13D840+0x11C));
    const auto second=static_cast<int32_t>(word(memory,0x13D980+0x11C));
    require(first>=0 && second>first,"published real pad socket identifiers");
    require(word(memory,0x13D840+0x120)==0 && word(memory,0x13D980+0x120)==0,"pad readiness starts unpolled");
    require(boot.pads().getState(first)==1 && polls==1 && boot.pads().getState(second)==0,"real input only on first port");
    auto &services=boot.services().services();
    require(!services.canBindRpc(0x80001300) && !services.canBindRpc(0x80000220),"no invented DBC or USB serial routes");
    require(log.find("unavailable")!=std::string::npos,"optional unsupported facilities explicitly logged");
    const auto temporary=boot.heap().allocate(12);
    require(temporary==0x10000 && boot.heap().release(temporary),"temporary descriptor allocation released");
    initializeScusPlatform(memory,boot,prepare,finish,sink);
    const auto replacement=static_cast<int32_t>(word(memory,0x13D840+0x11C));
    require(replacement>second && boot.pads().getState(first)==0,"reset closes old sockets without reusing identities");
    rejects([&]{initializeScusPlatform(memory,boot,{},finish,sink);},"missing lifecycle hook rejected");
    rejects([&]{initializeScusPlatform(memory,boot,prepare,{},sink);},"missing display hook rejected");
    require(boot.pads().getState(replacement)==1,"invalid configuration rejected before resetting current state");
    rejects([&]{initializeScusPlatform(memory,boot,prepare,[]{throw std::runtime_error("GPU failure");},sink);},"GPU failure cannot publish completion");
    require(boot.pads().getState(replacement)==0 && word(memory,0x13D840+0x11C)==UINT32_MAX,"failure leaves no live or stale socket");
    rejects([&]{initializeScusPlatform(memory,boot,prepare,finish,[](auto){return -1;});},"log failure propagates");
    require(boot.pads().statistics().created==boot.pads().statistics().closed,"failure cleans all created sockets");
    const auto reused=boot.heap().allocate(12);
    require(reused==0x10000 && boot.heap().release(reused),"failure releases descriptor allocation");
    DataMemory missingMemory; BootServices missing(missingMemory);
    missing.configureInput([]{return NativePadSample{};});
    missing.configureAssets(files,entries);
    rejects([&]{initializeScusPlatform(missingMemory,missing,prepare,finish,sink);},"unconfigured native heap fails");
    DataMemory absentMemory; BootServices absent(absentMemory);
    absent.heap().configure(0x10000,0x20000);
    absent.configureInput([]{return NativePadSample{};});
    const std::array<AssetEntry,1> absentEntries{{{"ABSENT.BIN",0,4}}};
    absent.configureAssets(files,absentEntries);
    rejects([&]{initializeScusPlatform(absentMemory,absent,prepare,finish,sink);},"missing required asset fails");
    require(!absent.services().ready(),"asset error precedes native service publication");
    DataMemory providerMemory; BootServices provider(providerMemory);
    provider.heap().configure(0x10000,0x20000);
    provider.configureAssets(files,entries);
    provider.services().configureControllerHooks([]{return false;},[]{});
    rejects([&]{initializeScusPlatform(providerMemory,provider,prepare,finish,sink);},"required controller provider failure propagates");
    bool anyActive=false;
    for(const auto &service:provider.services().services().debugSnapshot().services) anyActive=anyActive || service.active;
    require(word(providerMemory,0x13D840+0x11C)==UINT32_MAX && !anyActive,
            "failed provider publishes no socket or active services");
    DataMemory overlapMemory; BootServices overlap(overlapMemory);
    overlap.heap().configure(0x13D840,0x13DC00);
    overlap.configureAssets(files,entries);
    rejects([&]{initializeScusPlatform(overlapMemory,overlap,prepare,finish,sink);},"descriptor cannot overwrite source controller work");
    require(overlap.heap().allocate(12)==0x13D840,"overlap failure releases descriptor");
    std::printf("native platform bootstrap: %u checks passed\n",checks);
}
