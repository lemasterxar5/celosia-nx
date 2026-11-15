// SaltyNX FPS counter support for this homebrew forwarder.
//
// SaltyNX deliberately does not inject its core into homebrew titles (saltysd_proc hijack.c skips
// program ids above 0x01FFFFFFFFFFFFFF or with bits 0x1F00 set, which includes this forwarder), and
// its NX-FPS counter hooks NVN / EGL / Vulkan present imports that a statically linked deko3d
// program does not have. Overlays (Status Monitor, FPSLocker) read the counter from SaltyNX's
// shared memory page, finding the NX-FPS block by its magic.
//
// This file uses SaltyNX's own service, the same way its core and the overlays do: it connects to
// the "SaltySD" named port, asks for a block of the shared page (command 6,
// CheckIfSharedMemoryAvailable) and its handle (command 7, GetSharedMemoryHandle), ends the
// session at once (the port takes one session; overlays need it), maps the page and publishes the
// NX-FPS block from the renderer's real presents. Nothing is written if SaltyNX is not running.
// No game, renderer or clock behaviour depends on it.
#include <switch.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
// Field order of SaltyNX's NxFpsSharedBlock (NX-FPS-Common.hpp), packed. Overlays read up to
// expectedSetBuffers (174 bytes); the core's block is 310 bytes with its performance counters.
struct __attribute__((packed)) NxFpsBlock {
    uint32_t magic;
    uint8_t fps;
    float fpsAverage;
    bool pluginActive;
    uint8_t fpsLocked, fpsMode, zeroSync, patchApplied, api;
    uint32_t fpsTicks[10];
    uint8_t buffers, setBuffers, activeBuffers, setActiveBuffers;
    uint8_t displaySync;
    struct __attribute__((packed)) {uint16_t width, height, calls;} renderCalls[8], viewportCalls[8];
    bool forceOriginalRefreshRate, dontForce60InDocked, forceSuspend;
    uint8_t currentRefreshRate;
    float readSpeedPerSecond;
    uint8_t fpsLockedDocked;
    uint64_t frameNumber;
    int8_t expectedSetBuffers;
    uint8_t perfCounters[136];
};
static_assert(offsetof(NxFpsBlock, expectedSetBuffers) == 173, "NX-FPS overlay layout");
static_assert(sizeof(NxFpsBlock) == 310, "NX-FPS core layout");
constexpr uint32_t NxFpsMagic = 0x465053;
constexpr uint64_t TickFrequency = 19200000;

SharedMemory page{};
std::atomic<NxFpsBlock *> block{nullptr};
Thread connectThread;
alignas(0x1000) unsigned char connectStack[0x10000];

void log(const char *text, Result rc) {
    if (FILE *f = std::fopen("sdmc:/switch/celosia-native-boot/saltynx-fps.txt", "a")) {
        std::fprintf(f, "%s rc=0x%x\n", text, rc);
        std::fclose(f);
    }
}

void connect(void *) {
    log("thread started", 0);
    Handle port = INVALID_HANDLE;
    Result rc = 0;
    // The port takes a single session (svcManageNamedPort(..., "SaltySD", 1)): while SaltyNX or an
    // overlay holds it, connecting fails with kernel LimitReached (0x10801). Keep retrying in the
    // background (every 250 ms, up to 2 minutes); nothing in the game waits for this thread.
    int attempt = 0;
    Result first = 0;
    for (; attempt < 480; ++attempt) {
        rc = svcConnectToNamedPort(&port, "SaltySD");
        if (R_SUCCEEDED(rc)) break;
        if (!first) {first = rc; log("saltysd port busy or missing, retrying", rc);}
        if (R_VALUE(rc) != MAKERESULT(Module_Kernel, 132) && attempt >= 40) break; // missing port: give up after 10 s
        svcSleepThread(250'000'000ULL);
    }
    if (R_FAILED(rc)) {log("saltysd port not reachable, giving up", rc); return;}
    {
        char text[64];
        std::snprintf(text, sizeof(text), "port connected after %d retries", attempt);
        log(text, 0);
    }
    Service service;
    serviceCreate(&service, port);
    uint64_t offset = ~0ULL;
    const uint64_t size = sizeof(NxFpsBlock);
    rc = serviceDispatchInOut(&service, 6, size, offset, .in_send_pid = true);
    log("block request answered", rc);
    Handle memory = INVALID_HANDLE;
    Result handleRc = 0;
    if (R_SUCCEEDED(rc) && offset + size <= 0x1000)
        handleRc = serviceDispatch(&service, 7, .in_send_pid = true,
            .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &memory);
    log("handle request answered", handleRc);
    // EndSession (command 0), then release the port for the overlays.
    serviceDispatch(&service, 0, .in_send_pid = true);
    svcCloseHandle(port);
    if (R_FAILED(rc) || offset + size > 0x1000) {log("shared block reservation refused", rc); return;}
    if (R_FAILED(handleRc) || memory == INVALID_HANDLE) {log("shared memory handle refused", handleRc); return;}
    shmemLoadRemote(&page, memory, 0x1000, Perm_Rw);
    rc = shmemMap(&page);
    if (R_FAILED(rc)) {log("shared memory map failed", rc); return;}
    auto *published = reinterpret_cast<NxFpsBlock *>(static_cast<uint8_t *>(shmemGetAddr(&page)) + offset);
    std::memset(published, 0, sizeof(NxFpsBlock));
    published->currentRefreshRate = 60;
    published->magic = NxFpsMagic;
    block.store(published, std::memory_order_release);
    log("NX-FPS block published", 0);
}

}

// Started from main() once the SD card and runtime are up.
extern "C" void celosia_saltynx_start(void) {
    const Result rc = threadCreate(&connectThread, connect, nullptr, connectStack, sizeof(connectStack), 0x2C, -2);
    log("thread create", rc);
    if (R_SUCCEEDED(rc)) threadStart(&connectThread);
}

// Called by the deko3d backend once per presented image (render.cpp, weak reference).
extern "C" void celosia_frame_presented(void) {
    NxFpsBlock *b = block.load(std::memory_order_acquire);
    if (!b) return;
    static uint64_t previous = 0, windowStart = 0, average = 0;
    static uint32_t windowFrames = 0, tick = 0;
    const uint64_t now = armGetSystemTick();
    if (previous) {
        const uint64_t delta = now - previous;
        b->fpsTicks[tick] = uint32_t(delta > 0xFFFFFFFFu ? 0xFFFFFFFFu : delta);
        tick = (tick + 1) % 10;
        average = average ? (9 * average + delta) / 10 : delta;
        b->fpsAverage = float(TickFrequency) / float(average);
    } else windowStart = now;
    previous = now;
    ++windowFrames;
    // Presents in the last second, as NX-FPS counts them.
    if (now - windowStart >= TickFrequency) {
        b->fps = uint8_t(windowFrames > 255 ? 255 : windowFrames);
        windowFrames = 0;
        windowStart = now;
    }
    ++b->frameNumber;
    b->pluginActive = true;
}
