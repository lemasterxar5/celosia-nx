#pragma once
// Settings changed while the game runs (the in-game menu, Minus + Plus) and read by the deko3d
// backend at each frame's start, plus a few figures the backend publishes for the menu's overlay.
#include <atomic>
#include <cstdint>

namespace celosia::live {

struct Settings {
    // Presentation: 1 = every refresh (60 Hz cap), 2 = every second refresh (30 FPS cap).
    std::atomic<int> swapInterval{1};
    // Internal render height (16:9): 448, 540, 630, 720 (native panel), 900, 1080. Above 720 the
    // picture is drawn larger and scaled down to the 1280 x 720 output (supersampling).
    std::atomic<int> renderHeight{720};
    std::atomic<bool> bloom{true};
    std::atomic<bool> shadows{true};      // shadow volumes drawn at all
    std::atomic<bool> softShadows{true};
    std::atomic<bool> bodyShadows{true};  // the characters' shadows (Tuning::bodyShadows)  // soft (mask, reduce, blur) or hard (stencil darken)
    std::atomic<bool> addDiscard{true};   // additive fragments adding nothing are discarded
    std::atomic<bool> skyImage{true};     // the sky / far scenery as a background image (PS2's zFog)
    // Published by the backend: the last GPU frame time (us), presentation rate (centi-FPS) and
    // the render size in use.
    std::atomic<uint32_t> gpuUs{0}, fpsCenti{0}, renderWidthNow{1280}, renderHeightNow{720};
    // Save states: the menu asks (slot 1..3), the game thread answers at its next safe point.
    std::atomic<int> saveRequest{0}, loadRequest{0};
    std::atomic<int> stateResult{0}; // 0 idle, 1 saved, 2 loaded, negative: failure code
    std::atomic<bool> menuOpen{false};
};

inline Settings &settings() { static Settings value; return value; }

// 16:9 width for a render height (multiple of 4, as the 4 x 4 shadow reduction wants).
inline uint32_t widthFor(uint32_t height) { return ((height * 16u / 9u) + 3u) & ~3u; }

}
