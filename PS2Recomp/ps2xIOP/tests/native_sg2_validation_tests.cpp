#include "native_test_host.h"
#include "native/audio_arena.h"
#include "native/sg2_validation.h"
#include "emulator/services/iop_sg2.h"
#include "ps2x/iop/audio_stats.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>

// Fail allocations above the legitimate mixer/arena budget instead of ever
// exercising the old several-GiB reserve in a regression test.
static bool oversizedAllocation = false;
void *operator new(size_t size) {
    if (size > 8u * 1024u * 1024u) { oversizedAllocation = true; throw std::bad_alloc(); }
    if (auto *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
static unsigned checks = 0;
static void require(bool ok, const char *why) { ++checks; if (!ok) throw std::runtime_error(why); }
using namespace ps2x::iop;
using namespace ps2x::iop::detail;
using Command = std::array<uint32_t,4>;

static RpcRequest request(NativeTestHost &host, const Command &cmd)
{
    host.writeGuest(0x100, cmd.data(), 16);
    RpcRequest req; req.sid = Sg2Driver::Sid; req.send = {0x100,16}; req.receive = {0x400,400};
    return req;
}
class FailingAudioRam final : public AudioRam {
public:
    bool readRam(uint32_t, void *, size_t) const override { return false; }
    bool writeRam(uint32_t, const void *, size_t) override { return false; }
    bool ownsRamRange(uint32_t a, size_t n) const override { return a <= (2u<<20) && n <= (2u<<20)-a; }
};
int main(int argc, char **argv)
{
    NativeTestHost host;
    AudioArena ram;
    Sg2Driver driver(host, ram);
    if (argc > 1 && std::strcmp(argv[1], "--allocation-only") == 0) {
        const auto req = request(host, {0x13, 0x80000, 0, UINT32_MAX});
        bool rejected = false;
        try { rejected = !driver.handleRpc(req).handled; } catch (const std::bad_alloc &) {}
        require(rejected && !oversizedAllocation, "unsafe transfer tried to allocate before validation");
        return 0;
    }
    for (const auto cmd : {Command{0x13,0x80000,0,UINT32_MAX}, Command{0x14,0x80000,0,UINT32_MAX},
        Command{0x18,0xFF020000,0,0}, Command{0x20,0x1FFFF0,0x800,0},
        Command{0x20,0x80000,UINT32_MAX,0}, Command{UINT32_MAX,0,0,0},
        Command{1,48,0,0}, Command{0x19,48,0,0}, Command{0x20,0x80000,0x400,0}}) {
        require(!validSg2Command(cmd, ram), "invalid payload policy accepted");
        require(!driver.handleRpc(request(host, cmd)).handled, "invalid payload reached execution");
    }
    require(!oversizedAllocation, "oversized allocation attempted");
    const Command stream{0x18, 0x00021000, 0x40000100, 0x00080000};
    require(validSg2Command(stream, ram), "valid two-channel stream setup rejected");
    const Command edgeStream{0x18,0x01021000,0x40000400,0x001c0400};
    require(validSg2Command(edgeStream, ram), "right channel ending at arena boundary rejected");
    auto overflowStream = edgeStream; overflowStream[3] += 16;
    require(!validSg2Command(overflowStream, ram), "last interleaved read beyond arena accepted");
    auto badChannels = stream; badChannels[1] = 0x00031000;
    require(!validSg2Command(badChannels, ram), "unsupported interleave accepted");
    for (uint32_t code : {0u,0x12u,0x17u})
        require(driver.handleRpc(request(host, {code,0,0,0})).handled, "known no-op rejected");
    const auto before = audio_stats::keyOns.load();
    const std::array<uint32_t,8> batch{7,0,1,0, UINT32_MAX,0,0,0};
    host.writeGuest(0x100, batch.data(), sizeof(batch));
    RpcRequest invalidBatch; invalidBatch.sid = Sg2Driver::Sid;
    invalidBatch.send = {0x100,sizeof(batch)}; invalidBatch.receive = {0x400,400};
    require(!driver.handleRpc(invalidBatch).handled && audio_stats::keyOns.load() == before,
        "invalid batch mutated sound before validation completed");
    auto faultRequest = request(host, {7,0,1,0});
    host.failReadAt = faultRequest.send.address;
    require(!driver.handleRpc(faultRequest).handled && audio_stats::keyOns.load() == before,
        "failed command copy mutated sound or reported success");
    host.failReadAt = faultRequest.receive.address;
    require(!driver.handleRpc(faultRequest).handled && audio_stats::keyOns.load() == before,
        "failed reply preflight mutated sound or reported success");
    host.failReadAt = UINT32_MAX;
    faultRequest = request(host, {0,0,0,0});
    host.failWriteAt = faultRequest.receive.address;
    require(!driver.handleRpc(faultRequest).handled, "failed reply write reported completion");
    host.failWriteAt = UINT32_MAX;
    FailingAudioRam failing;
    Sg2Driver failedDriver(host, failing);
    require(!failedDriver.handleRpc(request(host, {0x20,0x80000,0x800,0})).handled,
        "failed PCM copy reported success");
    std::array<int16_t,128> output{};
    require(failedDriver.mixer().readOutput(output.data(),64) == 64, "native pull mode required for fixture");
    for (auto sample : output) require(sample == 0, "failed PCM copy leaked uninitialized samples");
    std::printf("SG2 validation: %u checks passed\n", checks);
}
