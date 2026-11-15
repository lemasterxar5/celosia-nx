#include "native_test_host.h"
#include "ps2x/iop/iop_subsystem.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <stdexcept>

static unsigned checks = 0;
static void require(bool ok, const char *message) { ++checks; if (!ok) throw std::runtime_error(message); }
int main()
{
    using namespace ps2x::iop;
#if defined(_WIN32)
    _putenv_s("SOTC_NATIVE", "0");
    _putenv_s("PS2X_NATIVE_IOP", "0");
#else
    setenv("SOTC_NATIVE", "0", 1); setenv("PS2X_NATIVE_IOP", "0", 1);
#endif
    NativeTestHost host;
    IopSubsystem services(host);
    const auto initial = services.debugSnapshot();
    require(initial.diagnostics.empty(), "fresh services have no failure diagnostics");
    for (const auto &service : initial.services)
        require(!service.active, "services remain dormant until their module is loaded");
    RpcAbiRequest abi;
    abi.boundSid = 0x80001300;
    require(services.selectRpcAbi(abi) == RpcAbi::RuntimeDefault,
        "unloaded services cannot override the caller ABI");
    int16_t idleSamples[2]{99,99};
    require(services.readAudio(idleSamples,1) == 0 && idleSamples[0] == 99,
        "dormant sound does not claim audio frames");
    require(services.readAudio(nullptr,1) == 0 && services.readAudio(idleSamples,0) == 0,
        "invalid device pulls are rejected");
    require(!services.stopModule(-1), "stopping an unknown module fails");
    constexpr char EmbeddedNullPath[] = "rom0:DBCMAN\0suffix";
    for (const auto &path : {std::string{}, std::string(EmbeddedNullPath,sizeof(EmbeddedNullPath)-1),
                            std::string("rom0:UNKNOWN_NATIVE_PROVIDER")}) {
        const auto failed = services.loadModule(path);
        require(failed.handled && failed.moduleId < 0, "invalid or unknown module cannot load");
    }
    const auto errors = services.debugSnapshot();
    require(!errors.diagnostics.empty(), "unsupported module failure is visible");
    services.onSifTransfer({}); // No active service may invoke a guest callback.
    require(services.enterNativeMode(), "native-only services cannot select an interpreter from environment");
    require(!services.canBindRpc(0x12345678), "unknown RPC must not pretend to bind");
    require(!services.canBindRpc(0x11223344), "sound service dormant before load");
    const auto loaded = services.loadModule("cdrom0:\\SG2IOPM1.IRX;1");
    require(loaded.moduleId > 0, "native SG2 loads without opening or executing IRX");
    require(services.canBindRpc(0x11223344), "native sound route active");
    const auto loadedAgain = services.loadModule("cdrom0:\\SG2IOPM1.IRX;1");
    require(loadedAgain.moduleId == loaded.moduleId, "repeat module load retains its identity");
    int32_t stopResult = -1;
    require(services.stopModule(loadedAgain.moduleId, &stopResult) && stopResult == 0,
        "one module reference can be released");
    require(services.canBindRpc(0x11223344), "another module reference retains the sound route");
    RpcRequest status;
    status.sid = 0x11223344;
    status.receive = {0x400, 400};
    require(services.handleRpc(status).handled, "native sound status RPC");
    auto malformed = status;
    malformed.send = {0x100, std::numeric_limits<uint32_t>::max()};
    require(!services.handleRpc(malformed).handled, "oversized command ring rejected before allocation");
    malformed.send = {0x100, 15};
    require(!services.handleRpc(malformed).handled, "partial command rejected");
    malformed.send = {65530, 16};
    require(!services.handleRpc(malformed).handled, "invalid command memory rejected");
    malformed = status; malformed.receive = {65530, 400};
    require(!services.handleRpc(malformed).handled, "invalid reply memory rejected");
    RpcRequest unknown; unknown.sid = 0x12345678;
    require(!services.handleRpc(unknown).handled, "unknown RPC cannot fall back");
    std::array<int16_t,128> samples;
    samples.fill(99);
    services.runEeCycles(std::numeric_limits<uint64_t>::max());
    require(services.readAudio(samples.data(), 64) == 64, "host audio clock produces native mixer output");
    require(services.readAudio(samples.data(), std::numeric_limits<size_t>::max()) == 0, "unbounded audio pull rejected");
    for (auto sample : samples) require(sample == 0, "idle native mixer silence");
    const auto memory = services.allocateMemory(32, 16);
    require(memory != 0, "native service buffer allocation");
    uint32_t input = 0x12345678, output = 0;
    require(services.writeMemory(memory, &input, sizeof(input)), "native transfer write");
    require(services.readMemory(memory, &output, sizeof(output)) && output == input, "native transfer read");
    require(services.isMemoryRange(memory,32), "allocated sample buffer is a valid native RAM range");
    require(services.zeroMemory(memory,32), "native RAM can be cleared");
    require(services.readMemory(memory,&output,sizeof(output)) && output == 0,
        "native clear changes the copied bytes");
    require(!services.zeroMemory(0x1F801000,4), "native clear cannot address device registers");
    require(!services.isMemoryRange(0x1F801000, 4), "no peripheral address space");
    require(services.freeMemory(memory), "native buffer free");
    const auto snapshot = services.debugSnapshot();
    require(snapshot.emulatorInstructions == 0 && snapshot.emulatorCycles == 0 &&
        snapshot.emulatorThreads == 0 && snapshot.emulatorLoadedModules == 0, "no interpreter state or instructions");
    require(services.stopModule(loaded.moduleId), "stop native service");
    require(!services.canBindRpc(0x11223344), "sound route removed");
    require(services.loadModuleBuffer(0x100).moduleId < 0, "unknown executable buffers rejected");
    require(services.loadModule(std::string(2048, 'X')).moduleId < 0, "oversized module path rejected");
    services.reset();
    require(!services.canBindRpc(0x11223344), "reset clears native routes");
    require(services.debugSnapshot().diagnostics.empty(), "reset clears failure diagnostics");
    const auto controller = services.loadModule("rom0:DBCMAN");
    require(controller.moduleId > 0, "native controller manager route");
    require(services.canBindRpc(0x80001300), "loaded controller exposes its RPC SID");
    require(services.selectRpcAbi(abi) == RpcAbi::RuntimeDefault,
        "controller keeps the runtime ABI contract");
    services.onSifTransfer({SifTransferKind::SetDma,SifTransferPhase::AfterCopy,0x100,0x80000,16});
    RpcRequest version;
    version.sid = 0x80001300; version.function = 0x80001363; version.receive = {0x800, 16};
    require(services.handleRpc(version).handled, "existing native DBCMAN version RPC");
    uint32_t reportedVersion = 0;
    require(host.readGuest(0x800,&reportedVersion,sizeof(reportedVersion)) && reportedVersion == 0x0310,
        "controller version is actually copied to the caller");
    const auto controllerSnapshot = services.debugSnapshot();
    bool sawController = false;
    for (const auto &service : controllerSnapshot.services) {
        if (service.name != "dbcman") continue;
        sawController = service.active;
        bool countedQuery = false;
        for (const auto &metric : service.metrics)
            if (metric.name == "version_queries") countedQuery = metric.value == 1;
        require(countedQuery, "controller diagnostics count the handled version query");
    }
    require(sawController, "controller diagnostics report its loaded state");
    require(services.stopModule(controller.moduleId) && !services.canBindRpc(0x80001300),
        "stopping controller removes the service route");
    require(!services.handleRpc(version).handled, "stopped controller cannot answer RPCs");
    std::printf("native-only services: %u checks passed\n", checks);
}
