// Save states on the Switch (v94): the menu asks for a slot (live_settings saveRequest /
// loadRequest); the EE scheduler's idle hook (no guest thread running) takes or loads the whole
// machine (PS2Runtime::saveMachineState). Files: sdmc:/switch/celosia_nx/states/slotN.state
// = "CNXZ", version, raw size, a zstd frame. The previous file of the slot is kept as .bak.
// Memory-card saves (mc0 / mc1) are never touched.
#include "live_settings.hpp"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <zstd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

extern "C" void celosia_native_after_state_load() __attribute__((weak));

namespace
{
    constexpr const char *kDir = "sdmc:/switch/celosia_nx/states";
    constexpr uint32_t kFileVersion = 1u;
    constexpr uint64_t kMaxWaitUs = 10000000u; // how long to wait for a safe switch point

    std::atomic<bool> g_writing{false};
    int g_tries = 0;
    uint64_t g_firstTryUs = 0;
    // v160: a load refused only for "not at a safe point" is retried at the next switch points (as a save
    // is), the slot read once and kept here meanwhile; nothing of the machine changes before that check.
    std::vector<uint8_t> g_pendingLoad;
    int g_pendingSlot = 0;

    std::string slotPath(int slot, const char *suffix = "")
    {
        return std::string(kDir) + "/slot" + std::to_string(slot) + ".state" + suffix;
    }

    uint64_t nowUs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    // v98: on the game thread (v97's std::thread threw there: std::terminate), streamed: zstd
    // writes through a 1 MB buffer straight to the file (no second state-sized buffer). Written
    // next to the slot, the old one kept as .bak, renamed into place.
    void writeSlot(int slot, const std::vector<uint8_t> &raw)
    {
        auto &live = celosia::live::settings();
        const uint64_t t0 = nowUs();
        mkdir("sdmc:/switch/celosia_nx", 0777);
        mkdir(kDir, 0777);
        const std::string tmp = slotPath(slot, ".tmp"), dst = slotPath(slot), bak = slotPath(slot, ".bak");
        bool ok = false;
        uint64_t fileBytes = 0;
        FILE *f = std::fopen(tmp.c_str(), "wb");
        ZSTD_CCtx *cctx = f ? ZSTD_createCCtx() : nullptr;
        if (f && cctx)
        {
            uint8_t header[16]{};
            std::memcpy(header, "CNXZ", 4);
            const uint32_t version = kFileVersion;
            const uint64_t rawSize = raw.size();
            std::memcpy(header + 4, &version, 4);
            std::memcpy(header + 8, &rawSize, 8);
            ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header);
            fileBytes = sizeof(header);
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1);
            ZSTD_CCtx_setPledgedSrcSize(cctx, raw.size());
            static uint8_t outBuf[1u << 20];
            ZSTD_inBuffer in{raw.data(), raw.size(), 0};
            for (bool done = false; ok && !done;)
            {
                ZSTD_outBuffer out{outBuf, sizeof(outBuf), 0};
                const size_t left = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
                if (ZSTD_isError(left))
                {
                    std::fprintf(stderr, "[state] slot %d: compression failed: %s\n", slot, ZSTD_getErrorName(left));
                    ok = false;
                    break;
                }
                ok = std::fwrite(outBuf, 1, out.pos, f) == out.pos;
                fileBytes += out.pos;
                done = left == 0;
            }
        }
        if (cctx)
            ZSTD_freeCCtx(cctx);
        if (f)
            ok = std::fclose(f) == 0 && ok;
        if (ok)
        {
            std::remove(bak.c_str());
            std::rename(dst.c_str(), bak.c_str()); // fails harmlessly when the slot was empty
            ok = std::rename(tmp.c_str(), dst.c_str()) == 0;
        }
        if (!ok)
            std::remove(tmp.c_str());
        std::fprintf(stderr, "[state] slot %d: %s raw=%llu KB file=%llu KB zstd+write=%.0f ms\n", slot,
                     ok ? "saved" : "WRITE FAILED", (unsigned long long)(raw.size() >> 10),
                     (unsigned long long)(fileBytes >> 10), (nowUs() - t0) / 1000.0);
        live.stateResult = ok ? 1 : -11;
    }

    bool readSlot(int slot, std::vector<uint8_t> &raw, int &code)
    {
        FILE *f = std::fopen(slotPath(slot).c_str(), "rb");
        if (!f)
        {
            code = -2;
            return false;
        }
        std::vector<uint8_t> file;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 16)
        {
            file.resize(static_cast<size_t>(size));
            if (std::fread(file.data(), 1, file.size(), f) != file.size())
                file.clear();
        }
        std::fclose(f);
        uint32_t version = 0;
        uint64_t rawSize = 0;
        if (file.size() > 16u)
        {
            std::memcpy(&version, file.data() + 4, 4);
            std::memcpy(&rawSize, file.data() + 8, 8);
        }
        if (file.size() <= 16u || std::memcmp(file.data(), "CNXZ", 4) != 0 || version != kFileVersion || rawSize > (512ull << 20))
        {
            code = -3;
            return false;
        }
        raw.resize(static_cast<size_t>(rawSize));
        const size_t got = ZSTD_decompress(raw.data(), raw.size(), file.data() + 16, file.size() - 16);
        if (ZSTD_isError(got) || got != raw.size())
        {
            code = -3;
            return false;
        }
        return true;
    }

    bool idleHook(PS2Runtime &runtime)
    {
        auto &live = celosia::live::settings();
        const int save = live.saveRequest.load(std::memory_order_acquire);
        const int load = live.loadRequest.load(std::memory_order_acquire);
        if (!save && !load)
            return false;
        if (g_writing.load(std::memory_order_acquire))
            return false; // the last save is still being written: wait for it
        std::string why;
        if (g_tries == 0 && (g_firstTryUs = nowUs()) != 0)
            std::fprintf(stderr, "[state] request (save %d load %d) at an idle point\n", save, load);
        if (!runtime.machineSafeForState(why))
        {
            ++g_tries;
            if (nowUs() - g_firstTryUs < kMaxWaitUs)
                return false;
            std::fprintf(stderr, "[state] gave up waiting for a safe point: %s\n", why.c_str());
            live.saveRequest = 0;
            live.loadRequest = 0;
            live.stateResult = -5;
            g_tries = 0;
            return false;
        }
        if (save)
        {
            const uint64_t t0 = nowUs();
            std::vector<uint8_t> raw;
            if (!runtime.saveMachineState(raw, why))
            {
                // v98: draining the VU1 / DMA workers can queue an interrupt between the check
                // and the save: try again at a later switch point, within the same time limit.
                ++g_tries;
                if (nowUs() - g_firstTryUs < kMaxWaitUs)
                    return false;
                std::fprintf(stderr, "[state] slot %d: not saved: %s\n", save, why.c_str());
                live.saveRequest = 0;
                live.stateResult = -5;
                g_tries = 0;
                return false;
            }
            live.saveRequest = 0;
            std::fprintf(stderr, "[state] slot %d: taken in %.1f ms (%llu KB) after %d tries\n", save, (nowUs() - t0) / 1000.0,
                         (unsigned long long)(raw.size() >> 10), g_tries);
            g_tries = 0;
            writeSlot(save, raw);
            return false; // the machine is unchanged
        }
        const uint64_t t0 = nowUs();
        std::vector<uint8_t> raw;
        int code = 0;
        if (g_pendingSlot == load && !g_pendingLoad.empty())
            raw.swap(g_pendingLoad);
        else if (!readSlot(load, raw, code))
        {
            live.loadRequest = 0;
            g_tries = 0;
            g_pendingSlot = 0;
            std::fprintf(stderr, "[state] slot %d: cannot read (%d)\n", load, code);
            live.stateResult = code;
            return false;
        }
        g_pendingSlot = 0;
        const uint64_t t1 = nowUs();
        std::string error;
        const bool loaded = runtime.loadMachineState(raw.data(), raw.size(), error);
        if (!loaded && error.rfind("not at a safe point", 0) == 0 && nowUs() - g_firstTryUs < kMaxWaitUs)
        {
            ++g_tries; // the request stays: the next switch point tries again
            g_pendingLoad.swap(raw);
            g_pendingSlot = load;
            return false;
        }
        live.loadRequest = 0;
        if (loaded && g_tries)
            std::fprintf(stderr, "[state] slot %d: loaded after %d tries\n", load, g_tries);
        g_tries = 0;
        if (!loaded)
        {
            std::fprintf(stderr, "[state] slot %d: load failed: %s\n", load, error.c_str());
            // The header checks run before anything changes; past them the machine is half
            // replaced and cannot go on.
            if (error.rfind("state version", 0) == 0 || error.rfind("state from", 0) == 0 ||
                error.rfind("different game", 0) == 0 || error.rfind("not at a safe point", 0) == 0 ||
                error.rfind("expected section CNXS", 0) == 0)
            {
                live.stateResult = -3;
                return false;
            }
            std::fflush(stderr);
            runtime.requestStop();
            return true;
        }
        if (celosia_native_after_state_load)
            celosia_native_after_state_load();
        std::fprintf(stderr, "[state] slot %d: loaded (read+unzstd %.0f ms, restore %.0f ms)\n", load, (t1 - t0) / 1000.0,
                     (nowUs() - t1) / 1000.0);
        live.stateResult = 2;
        return true;
    }
}

extern "C" void celosia_states_install()
{
    EeScheduler::s_idleHook = idleHook;
    EeScheduler::s_stateWanted = []
    {
        auto &live = celosia::live::settings();
        return live.saveRequest.load(std::memory_order_relaxed) != 0 || live.loadRequest.load(std::memory_order_relaxed) != 0;
    };
}
