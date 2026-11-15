#pragma once
#include "audio_ram.h"
#include "sound_mixer.h"
#include <array>
#include <optional>
#include <numeric>

namespace ps2x::iop::detail {
constexpr uint32_t Sg2MaxCommands = 512;
constexpr uint32_t Sg2VoiceCount = 48;
constexpr uint32_t Sg2PcmBlock = 0x400;
constexpr uint32_t Sg2Interleave = 0x800;
using Sg2Command = std::array<uint32_t,4>;
inline bool validSg2Transfer(uint32_t address, uint32_t size, const AudioRam &ram)
{
    return size <= SoundMixer::RamBytes && (!size || ram.ownsRamRange(address, size));
}
inline bool validSg2Refill(uint32_t voice, uint32_t channels, uint32_t bytes,
                         uint32_t source, const AudioRam &ram)
{
    if (voice >= Sg2VoiceCount || !channels || Sg2Interleave % channels) return false;
    const uint32_t chunk = Sg2Interleave / channels;
    if (chunk < 16 || chunk % 16 || !bytes || bytes > SoundMixer::RamBytes || bytes % chunk)
        return false;
    const uint64_t span = uint64_t(bytes / chunk - 1u) * Sg2Interleave + chunk;
    return span <= SoundMixer::RamBytes && ram.ownsRamRange(source, static_cast<size_t>(span));
}
struct Sg2StreamSetup {
    uint32_t index, channels, source, length, soundAddress, size;
};
inline std::optional<Sg2StreamSetup> decodeSg2Stream(const Sg2Command &cmd, const AudioRam &ram)
{
    const auto a = cmd[1], b = cmd[2], c = cmd[3];
    const Sg2StreamSetup s{a >> 24, (a >> 16) & 0xFFu, c & 0xFFFFFFu,
        ((b & 0xFFFFu) << 8) | (c >> 24), ((a & 0xFFu) << 16) | (b >> 16), a & 0xFF00u};
    if (!s.length || s.length > SoundMixer::RamBytes ||
        s.soundAddress >= SoundMixer::RamBytes || s.size > SoundMixer::RamBytes - s.soundAddress ||
        !validSg2Refill(s.index, s.channels, s.size / 2, s.source, ram)) return std::nullopt;
    const uint64_t needed = uint64_t(s.size / 2) * s.channels;
    if (needed > s.length) return std::nullopt;
    // source already includes the channel offset. Validate the furthest actual
    // strided read over the modulo loop, rather than adding the whole ring size.
    const uint32_t chunk = Sg2Interleave / s.channels;
    const uint64_t span = uint64_t((s.size / 2) / chunk - 1u) * Sg2Interleave + chunk;
    const uint32_t lastPosition = s.length - std::gcd(static_cast<uint32_t>(needed), s.length);
    if (!ram.ownsRamRange(s.source, lastPosition + span)) return std::nullopt;
    return s;
}
inline bool validSg2Command(const Sg2Command &cmd, const AudioRam &ram)
{
    const auto opcode = cmd[0], a = cmd[1], b = cmd[2], c = cmd[3];
    if (opcode > 0x23u) return false;
    switch (opcode) {
    case 1: case 2: case 3: case 4: case 0x19: return a < Sg2VoiceCount;
    case 5: return a == 1 || (a == 0 && b < Sg2VoiceCount);
    case 6: case 0xB: case 0x15: return (a & 0xFFFFu) < 2;
    case 0xC: case 0xD: case 0xE: case 0xF: case 0x10: return a < 2;
    case 0x11: case 0x23: return a <= 1;
    case 0x13: case 0x14: return validSg2Transfer(a, c, ram);
    case 0x18: return decodeSg2Stream(cmd, ram).has_value();
    case 0x1B: return c <= INT32_MAX; // Signed rate; multiplication uses int64 in the driver.
    case 0x20:
        return b >= 2 * Sg2PcmBlock && b % Sg2PcmBlock == 0 && validSg2Transfer(a, b, ram);
    default: return true; // Known mask/volume commands, and explicit no-ops 0,12,17.
    }
}
}
