#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"
#include "native/sound_mixer.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class AudioRam;

    // Native stand-in for SotC's IOP sound driver (SG2IOPM1.IRX,
    // "sg2iop_driver", RPC server 0x11223344): the EE's SG2 engine sends it
    // rings of 16-byte commands and reads a 400-byte status back. The driver
    // turns them into libsd calls; here they go straight to a host mixer
    // (SoundMixer): neither the IRX, libsd nor the emulated SPU2 is used.
    // PS2X_NATIVE_SG2=1 turns it on (default with SOTC_NATIVE=1).
    class Sg2Driver
    {
    public:
        static constexpr uint32_t Sid = 0x11223344u;

        Sg2Driver(IopHost &host, AudioRam &memory);

        [[nodiscard]] static bool enabled();
        // True for the driver's IRX image (its .iopmod name).
        [[nodiscard]] static bool isDriverImage(std::span<const uint8_t> image);

        void reset();
        // Direct native control path: bounded commands and a complete staged
        // status reply. No RPC transport, game-memory read/write or callbacks.
        // All commands validate before effects; reply changes only on success.
        // Operational failures can follow prior command effects: callers must
        // surface failure, never acknowledge or silently retry that batch.
        [[nodiscard]] bool submitCommandBatch(std::span<const std::array<uint32_t,4>> commands,
                                               std::span<uint8_t,400> reply);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        [[nodiscard]] SoundMixer &mixer() { return m_mixer; }

        // Save states (v94): the sound driver (streams, their queued refills, effects, PCM, the
        // volumes it keeps) and its mixer.
        template <typename W>
        void saveState(W &out)
        {
            m_mixer.saveState(out);
            out.pod(m_eea); out.pod(m_coreVolumes); out.pod(m_voiceVolumes); out.pod(m_reply); out.pod(m_streams);
            out.pod(m_streamOps); out.pod(m_opWrite); out.pod(m_opRead); out.pod(m_pendingAdvance); out.pod(m_effects);
            out.pod(m_transfer); out.pod(m_ioFailed); out.pod(m_pcmBuffer); out.pod(m_pcmSize); out.pod(m_pcmPosition);
            out.pod(m_pcmFirst); out.pod(m_pcmOn); out.pod(m_pcmVolL); out.pod(m_pcmVolR);
            for (const auto &m : m_sourceHash)
            {
                out.u32(static_cast<uint32_t>(m.size()));
                for (const auto &kv : m) { out.u32(kv.first); out.u64(kv.second); }
            }
            out.u32(static_cast<uint32_t>(m_pcmHash.size()));
            for (uint64_t h : m_pcmHash) out.u64(h);
        }
        template <typename R>
        bool loadState(R &in)
        {
            if (!m_mixer.loadState(in)) return false;
            m_eea = in.template pod<decltype(m_eea)>();
            in.raw(m_coreVolumes, sizeof(m_coreVolumes)); in.raw(m_voiceVolumes, sizeof(m_voiceVolumes));
            m_reply = in.template pod<decltype(m_reply)>(); m_streams = in.template pod<decltype(m_streams)>();
            m_streamOps = in.template pod<decltype(m_streamOps)>(); m_opWrite = in.u32(); m_opRead = in.u32();
            m_pendingAdvance = in.u32(); m_effects = in.template pod<decltype(m_effects)>(); m_transfer = in.template pod<int>();
            m_ioFailed = in.template pod<bool>(); m_pcmBuffer = in.u32(); m_pcmSize = in.u32(); m_pcmPosition = in.u32();
            m_pcmFirst = in.template pod<bool>(); m_pcmOn = in.template pod<bool>();
            m_pcmVolL = in.template pod<uint16_t>(); m_pcmVolR = in.template pod<uint16_t>();
            for (auto &m : m_sourceHash)
            {
                m.clear();
                const uint32_t n = in.u32();
                if (n > (1u << 20)) { in.fail("bad SG2 hash size"); return false; }
                for (uint32_t i = 0; i < n && in.ok(); ++i) { const uint32_t k = in.u32(); m[k] = in.u64(); }
            }
            const uint32_t n = in.u32();
            if (n > (1u << 22)) { in.fail("bad PCM hash size"); return false; }
            m_pcmHash.assign(n, 0);
            for (uint32_t i = 0; i < n && in.ok(); ++i) m_pcmHash[i] = in.u64();
            return in.ok();
        }

    private:
        // A voice streamed from IOP memory through a double buffer in sound
        // RAM (driver table 0x7218, 0x24 bytes per voice).
        struct Stream
        {
            int32_t state = 0;
            int32_t channels = 0; // interleave: 0x800 / channels bytes per 0x800 of source
            int32_t spuAddress = 0;
            int32_t size = 0; // both halves, bytes
            int32_t source = 0; // IOP address
            int32_t length = 0; // source loop length
            int32_t position = 0;
            int32_t half = 0, queuedHalf = 0;
            // PS2X_SG2_STREAM_LOG: the half the voice was last seen in, and
            // which halves got new data since the voice last left them.
            int32_t seenHalf = 0;
            bool fresh[3] = {false, false, false};
            // audio_stats: paused by the game since (host us, 0: playing).
            uint64_t stalledSince = 0;
        };
        uint64_t m_logRefills = 0, m_logStale = 0, m_logEntries = 0, m_logStatus = 0;
        double m_logLast = 0.0;
        // Driver queue 0x78d8: 1 refill, 2 key on, 3 key off.
        struct StreamOp
        {
            int32_t op = 0;
            uint32_t arg[6]{};
            uint64_t queuedUs = 0; // audio_stats: refill wait
        };
        // sceSdEffectAttr per core (0x4c48).
        struct Effect
        {
            uint32_t mode = 0;
            int16_t depthL = 0, depthR = 0;
            int32_t delay = 0, feedback = 0;
        };

        void runCommand(const uint32_t *command);
        void failIo(const char *message);
        void fillReply();
        void streamTick();
        void runStreamQueue();
        void queueStream(int32_t op, uint32_t a0, uint32_t a1, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0, uint32_t a5 = 0);
        void dropStreamOps(uint32_t voice);
        void refill(const StreamOp &op);
        void streamKey(const StreamOp &op, bool on);
        void setEffect(int core);
        void pcmStart(uint32_t buffer, uint32_t size);
        void pcmStop();
        void pcmRefill();
        void countPitch(uint32_t mask0, uint32_t mask1, uint32_t rate);
        void countStop(uint32_t mask0, uint32_t mask1);

        // libsd equivalents (parameter encodings as the driver passes them).
        void setParam(uint32_t param, uint16_t value);
        [[nodiscard]] uint16_t getParam(uint32_t param);
        void setSwitch(uint32_t entry, uint32_t mask);
        void setAddr(uint32_t entry, uint32_t byteAddress);
        [[nodiscard]] uint32_t getAddr(uint32_t entry);
        void setCoreAttr(uint32_t entry, uint32_t value);
        void sdInit();
        void setCoreVolume(uint32_t core, uint32_t index, uint16_t value);
        void iopToSpu(uint32_t iopAddress, uint32_t spuAddress, uint32_t bytes);

        IopHost &m_host;
        AudioRam &m_memory;
        SoundMixer m_mixer;
        std::array<uint32_t, 2> m_eea{}; // effect area end, words
        uint16_t m_coreVolumes[2][8]{};
        uint16_t m_voiceVolumes[48][2]{};

        std::array<uint8_t, 400> m_reply{};
        std::array<Stream, 48> m_streams{};
        std::array<StreamOp, 128> m_streamOps{};
        uint32_t m_opWrite = 0, m_opRead = 0;
        uint32_t m_pendingAdvance = 0; // voice | 0x10000 after a refill
        std::array<Effect, 2> m_effects{};
        int m_transfer = 0; // 0x7080: 0 idle, 1 running, 2 done
        bool m_ioFailed = false; // Protected by the mixer lock, including refill callbacks.
        // PCM input (0xc8e8..): buffer, size, position, first-refill flag, BVOL.
        uint32_t m_pcmBuffer = 0, m_pcmSize = 0, m_pcmPosition = 0;
        bool m_pcmFirst = false, m_pcmOn = false;
        uint16_t m_pcmVolL = 0, m_pcmVolR = 0;
        // audio_stats: the bytes last played from each stream source
        // address (per voice) and each PCM block (stale data checks).
        std::array<std::unordered_map<uint32_t, uint64_t>, 48> m_sourceHash{};
        std::vector<uint64_t> m_pcmHash;
        uint64_t m_lastStatusUs = 0;
    };
}
