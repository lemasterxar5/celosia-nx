// SaltyNX FPS counter support for this homebrew forwarder.
//
// SaltyNX does not inject its core (and NX-FPS) into this process, and NX-FPS hooks NVN / EGL /
// Vulkan presents that a statically linked deko3d program does not have. Overlays (Status Monitor,
// FPSLocker) read the counter from SaltyNX's 0x1000-byte shared page, finding the NX-FPS block by
// its magic, and treat the game as running when pluginActive comes back true within 100 ms after
// they clear it.
//
// This file talks to SaltyNX's own "SaltySD" service like its core and the overlays do: command 7
// (GetSharedMemoryHandle) to map the page, command 6 (CheckIfSharedMemoryAvailable) to reserve a
// block only if no NX-FPS block is present yet, then command 0 (EndSession) at once, because the
// port takes a single session. A publisher thread writes the counter from the renderer's real
// presents. Nothing is written if SaltyNX is not running; no game, renderer or clock behaviour
// depends on it.
//
// svcConnectToNamedPort fails with LimitReached (0x10801) when this process has used up its own
// session resource limit (applications have few sessions and the runtime already holds many). Before
// connecting, the session limit is checked; if it is full, the limit is raised when the syscall is
// available, and otherwise (only for the first attempt, made from main() before the runtime starts
// any thread) the sm session is released for the duration of the connection and restored after.
// The same technique is used by other deko3d Switch ports (e.g. NaGaa95's NetherSX2_nx saltynx.cpp).
#include <switch.h>
#include "live_settings.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
// Field order of SaltyNX's NxFpsSharedBlock (NX-FPS-Common.hpp), packed. Overlays read up to
// expectedSetBuffers (174 bytes); the core reserves 310 bytes with its performance counters.
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
};
static_assert(offsetof(NxFpsBlock, fpsTicks) == 15, "NX-FPS overlay layout");
static_assert(offsetof(NxFpsBlock, renderCalls) == 60, "NX-FPS overlay layout");
static_assert(offsetof(NxFpsBlock, frameNumber) == 165, "NX-FPS overlay layout");
static_assert(sizeof(NxFpsBlock) == 174, "NX-FPS overlay layout");
constexpr uint32_t NxFpsMagic = 0x465053;
// v151: ReverseNX-RT's block (its overlay's Overlay/source/main.cpp, 9 bytes packed). The game publishes
// it; the overlay sets def = false and isDocked to force a mode, or def = true for the console's own.
struct __attribute__((packed)) NxRtBlock {
    uint32_t magic;
    bool isDocked, def, pluginActive;
    uint8_t res;        // handheld_res : 4, docked_res : 4 (ReverseNX's resolution menu; not used)
    bool wasDDRused;
};
static_assert(sizeof(NxRtBlock) == 9, "ReverseNX-RT layout");
constexpr uint32_t NxRtMagic = 0x5452584E; // "NXRT"
constexpr uint64_t NxRtReserve = 12;       // 9 rounded to the 4-byte scan step
constexpr uint64_t ReserveBytes = 310; // the core's full block size
constexpr size_t PageBytes = 0x1000;
constexpr uint8_t ApiDeko3d = 0;          // NX-FPS api field: 0 = unknown/other

SharedMemory page{};
uint8_t *pageBase = nullptr;
volatile NxFpsBlock *block = nullptr;     // owned by the publisher thread after start
std::atomic<volatile NxRtBlock *> rtBlock{nullptr};
Thread publisherThread;
alignas(0x1000) unsigned char publisherStack[0x8000];
int logLines = 0;

// Present history, written by the render thread (celosia_frame_presented).
std::atomic<uint64_t> presents{0}, lastPresentTick{0};
std::array<std::atomic<uint32_t>, 10> presentTicks{};

void log(const char *text, Result rc) {
    if (logLines >= 40) return;
    ++logLines;
    if (FILE *f = std::fopen("sdmc:/switch/celosia_nx/saltynx-fps.txt", "a")) {
        std::fprintf(f, "%s rc=0x%x\n", text, rc);
        std::fclose(f);
    }
}

volatile NxRtBlock *findRt() {
    if (!pageBase) return nullptr;
    for (size_t offset = 0; offset + sizeof(NxRtBlock) <= PageBytes; offset += 4) {
        uint32_t value;
        std::memcpy(&value, pageBase + offset, sizeof(value));
        if (value == NxRtMagic) return reinterpret_cast<volatile NxRtBlock *>(pageBase + offset);
    }
    return nullptr;
}

volatile NxFpsBlock *findBlock() {
    if (!pageBase) return nullptr;
    for (size_t offset = 0; offset + sizeof(NxFpsBlock) <= PageBytes; offset += 4) {
        uint32_t value;
        std::memcpy(&value, pageBase + offset, sizeof(value));
        if (value == NxFpsMagic) return reinterpret_cast<volatile NxFpsBlock *>(pageBase + offset);
    }
    return nullptr;
}

// True when the caller must release sm around the connection (no free session and no way to raise
// the limit). Only the early attempt may do that.
bool sessionsFull() {
    uint64_t raw = 0;
    if (R_FAILED(svcGetInfo(&raw, InfoType_ResourceLimit, INVALID_HANDLE, 0)) || !raw) return false;
    const Handle limit = Handle(raw);
    s64 current = 0, maximum = 0;
    bool full = R_SUCCEEDED(svcGetResourceLimitCurrentValue(&current, limit, LimitableResource_Sessions)) &&
        R_SUCCEEDED(svcGetResourceLimitLimitValue(&maximum, limit, LimitableResource_Sessions)) && current >= maximum;
    if (full && envIsSyscallHinted(0x7E)) {
        const Result rc = svcSetResourceLimitLimitValue(limit, LimitableResource_Sessions, maximum + 4);
        log("session limit full; raising it", rc);
        if (R_SUCCEEDED(rc)) full = false;
    }
    svcCloseHandle(limit);
    return full;
}

bool attach(bool early) {
    if (pageBase && (block = findBlock()) && rtBlock.load()) return true;
    const bool releaseSm = sessionsFull() && early;
    if (releaseSm) smExit();
    Handle port = INVALID_HANDLE;
    Result rc = svcConnectToNamedPort(&port, "SaltySD");
    if (R_SUCCEEDED(rc)) {
        Service service{};
        serviceCreate(&service, port);
        if (!pageBase) {
            Handle memory = INVALID_HANDLE;
            const Result handleRc = serviceDispatch(&service, 7, .in_send_pid = true,
                .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &memory);
            if (R_SUCCEEDED(handleRc) && memory != INVALID_HANDLE) {
                shmemLoadRemote(&page, memory, PageBytes, Perm_Rw);
                const Result mapRc = shmemMap(&page);
                if (R_SUCCEEDED(mapRc)) pageBase = static_cast<uint8_t *>(shmemGetAddr(&page));
                else {shmemClose(&page); log("shared memory map failed", mapRc);}
            } else log("shared memory handle refused", handleRc);
        }
        block = findBlock();
        if (pageBase && !block) {
            uint64_t offset = ~0ULL;
            const Result reserveRc = serviceDispatchInOut(&service, 6, ReserveBytes, offset, .in_send_pid = true);
            if (R_SUCCEEDED(reserveRc) && offset <= PageBytes - ReserveBytes && offset % 4 == 0) {
                std::memset(pageBase + offset, 0, ReserveBytes);
                block = reinterpret_cast<volatile NxFpsBlock *>(pageBase + offset);
                block->currentRefreshRate = 60;
                block->magic = NxFpsMagic;
            } else log("shared block reservation refused", reserveRc);
        }
        if (pageBase && !rtBlock.load()) {
            volatile NxRtBlock *rt = findRt();
            if (!rt) {
                uint64_t offset = ~0ULL;
                const Result reserveRc = serviceDispatchInOut(&service, 6, NxRtReserve, offset, .in_send_pid = true);
                if (R_SUCCEEDED(reserveRc) && offset <= PageBytes - NxRtReserve && offset % 4 == 0) {
                    std::memset(pageBase + offset, 0, NxRtReserve);
                    rt = reinterpret_cast<volatile NxRtBlock *>(pageBase + offset);
                    rt->def = true;
                    rt->isDocked = appletGetOperationMode() == AppletOperationMode_Console;
                    rt->magic = NxRtMagic;
                    log("ReverseNX-RT block published", 0);
                } else log("ReverseNX-RT block reservation refused", reserveRc);
            }
            rtBlock.store(rt);
        }
        const uint64_t zero = 0;
        serviceDispatchIn(&service, 0, zero, .in_send_pid = true); // EndSession: free the port for overlays
        svcCloseHandle(port);
    }
    if (releaseSm) smInitialize();
    if (block) log(early ? "NX-FPS block published (early)" : "NX-FPS block published", 0);
    else if (early) log("saltysd not reachable yet, retrying in background", rc);
    return block != nullptr;
}

void publish() {
    if (!block || block->magic != NxFpsMagic) return;
    const uint64_t frequency = armGetSystemTickFreq();
    const uint64_t last = lastPresentTick.load(std::memory_order_relaxed);
    const bool recent = last && armGetSystemTick() - last < frequency;
    uint64_t sum = 0;
    unsigned count = 0;
    for (const auto &sample : presentTicks)
        if (const auto ticks = sample.load(std::memory_order_relaxed)) {sum += ticks; ++count;}
    const float average = recent && sum ? float(double(frequency) * count / double(sum)) : 0.0f;
    const uint32_t rounded = average > 0 ? uint32_t(average + 0.5f) : 0;
    const uint32_t fallback = rounded ? uint32_t(frequency / rounded) : uint32_t(frequency);
    for (unsigned i = 0; i < presentTicks.size(); ++i) {
        const auto ticks = presentTicks[i].load(std::memory_order_relaxed);
        block->fpsTicks[i] = recent && ticks ? ticks : fallback;
    }
    block->fps = uint8_t(std::min(rounded, 255u));
    block->fpsAverage = average;
    block->api = ApiDeko3d;
    block->frameNumber = presents.load(std::memory_order_acquire);
    for (unsigned i = 0; i < 8; ++i) block->renderCalls[i].calls = block->viewportCalls[i].calls = 0;
    const auto &live = celosia::live::settings();
    block->renderCalls[0].width = block->viewportCalls[0].width = uint16_t(live.renderWidthNow.load(std::memory_order_relaxed));
    block->renderCalls[0].height = block->viewportCalls[0].height = uint16_t(live.renderHeightNow.load(std::memory_order_relaxed));
    block->renderCalls[0].calls = block->viewportCalls[0].calls = uint16_t(std::clamp(rounded, 1u, 65534u));
    // Overlays clear this and expect it back within 100 ms while the game runs.
    block->pluginActive = recent;
}

void publisher(void *) {
    unsigned attempts = 0;
    for (;;) {
        if ((!block || block->magic != NxFpsMagic) && attempts++ % 20 == 0) attach(false); // about once a second
        publish();
        svcSleepThread(50'000'000ULL);
    }
}
}

// Called from main() once the SD card is up and before the runtime starts its threads.
extern "C" void celosia_saltynx_start(void) {
    attach(true);
    const Result rc = threadCreate(&publisherThread, publisher, nullptr, publisherStack, sizeof(publisherStack), 0x3B, -2);
    if (R_SUCCEEDED(rc)) threadStart(&publisherThread);
    else log("publisher thread create failed", rc);
}

// Called by the deko3d backend once per presented image (render.cpp, weak reference).
// v151: the mode the game runs in: ReverseNX-RT's when it controls it (def false), else the console's
// (written back so the overlay shows it). pluginActive tells the overlay the game reads the mode.
extern "C" int celosia_effective_docked(int real) {
    volatile NxRtBlock *rt = rtBlock.load(std::memory_order_acquire);
    if (!rt || rt->magic != NxRtMagic) return real;
    rt->pluginActive = true;
    rt->wasDDRused = true;
    if (rt->def) {rt->isDocked = real != 0; return real;}
    return rt->isDocked ? 1 : 0;
}

extern "C" void celosia_frame_presented(void) {
    const uint64_t now = armGetSystemTick();
    const uint64_t previous = lastPresentTick.exchange(now, std::memory_order_relaxed);
    const uint64_t index = presents.fetch_add(1, std::memory_order_acq_rel);
    if (previous && now > previous) {
        const uint64_t delta = now - previous;
        if (delta >= armGetSystemTickFreq()) for (auto &ticks : presentTicks) ticks.store(0, std::memory_order_relaxed);
        else presentTicks[index % presentTicks.size()].store(uint32_t(delta), std::memory_order_relaxed);
    }
}
