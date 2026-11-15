#include "native_test_host.h"
#include "native/audio_arena.h"
#include "emulator/services/iop_sg2.h"
#include <cstdio>
#include <stdexcept>

int main()
{
    using namespace ps2x::iop;
    NativeTestHost host;
    detail::AudioArena arena;
    detail::Sg2Driver driver(host, arena);
    RpcRequest status;
    status.sid = detail::Sg2Driver::Sid;
    status.receive = {0x400, 400};
    const auto reply = driver.handleRpc(status);
    if (!reply.handled || !reply.signalCompletion ||
        reply.serverDispatchPolicy != ServerDispatchPolicy::Suppress)
        throw std::runtime_error("native SG2 status did not complete directly");
    bool changed = false;
    for (size_t i = 0; i < 400; ++i) changed |= host.guest[0x400 + i] != 0xAA;
    if (!changed) throw std::runtime_error("native SG2 status was not written");
    std::array<int16_t,128> stereo;
    stereo.fill(123);
    driver.mixer().advanceTo(0); // Establish the clock epoch before asking it to advance.
    driver.mixer().advanceTo(768 * 64);
    if (!detail::SoundMixer::pullMode())
    {
        if (driver.mixer().readOutput(stereo.data(), 64) != 0)
            throw std::runtime_error("clocked mixer must respect its 2048-frame prebuffer");
        driver.mixer().advanceTo(768 * 2048);
    }
    const auto frames = driver.mixer().readOutput(stereo.data(), 64);
    if (!frames) throw std::runtime_error("native mixer did not produce clocked output");
    for (size_t i = 0; i < frames * 2; ++i)
        if (stereo[i] != 0) throw std::runtime_error("idle native driver must produce silence");
    std::printf("native SG2: direct status and %zu silent stereo frames passed\n", frames);
}
