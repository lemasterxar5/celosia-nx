#pragma once
// Settings changed while the game runs (the in-game menu, Minus + Plus) and read by the deko3d
// backend at each frame's start, plus a few figures the backend publishes for the menu's overlay.
#include <atomic>
#include <cstdint>

namespace celosia::live {

struct Settings {
    // Presentation: 1 = every refresh (60 Hz cap), 2 = every second refresh (30 FPS cap).
    std::atomic<int> swapInterval{1};
    // v149: the real frame cap (30, 45 or 60 FPS): presents of one, one and two refreshes alternate at 45.
    std::atomic<int> fpsCap{30};
    // v149: anisotropic filtering of the scene's textures (1 = off, 2, 4, 8, 16), applied at a frame's start.
    std::atomic<int> anisotropy{1};
    // Internal render height (16:9): 448, 540, 630, 720 (native panel), 900, 1080. Above 720 the
    // picture is drawn larger and scaled down to the 1280 x 720 output (supersampling).
    std::atomic<int> renderHeight{720};
    // v150: the render height chosen for each mode (menu sliders; the console's mode picks one).
    std::atomic<int> handheldHeight{720}, dockedHeight{1080};
    // v151: the effective mode the renderer uses (console or ReverseNX-RT), for the menu.
    std::atomic<bool> docked{false};
    // v152: image options, hot: anti-aliasing (0 off, 1 FXAA, 2 MSAA 2x, 3 MSAA 4x, 4 MSAA 4x + FXAA),
    // sharpening 0..3, texture detail (mip bias 0, -0.5, -1), upscaling filter (0 bilinear, 1 bicubic),
    // debanding (a fine dither on the final picture).
    std::atomic<int> antiAliasing{0}, sharpen{0}, textureDetail{0}, upscaleFilter{0};
    std::atomic<bool> deband{false};
    std::atomic<bool> msaaFailed{false};
    // v153: frame generation (LSFG): 0 off, 1 = 60 (30 x 2), 2 = 90 (45 x 2), 3 = 120 (60 x 2); flow quality
    // 0 performance (flow at 1/2), 1 balanced (3/4), 2 quality (full). frameGenState: 0 ok, 1 shaders missing
    // from the SD card, 2 not enough memory. frameGenReal: real frames per second x 100 while it runs.
    std::atomic<int> frameGen{0}, frameGenQuality{0}, frameGenState{0};
    std::atomic<bool> frameGenPerformance{true}; // v173m: lsfg performance network (Cemu-nx default)
    std::atomic<uint32_t> frameGenReal{0};
    // v155: the display's refresh period measured on its vsync event (microseconds), and the images shown
    // per second x 100 while frame generation runs (generated + real).
    std::atomic<uint32_t> refreshUs{16667}, frameGenShown{0};
    std::atomic<bool> bloom{true};
    std::atomic<bool> shadows{true};      // shadow volumes drawn at all
    std::atomic<bool> softShadows{true};
    std::atomic<bool> bodyShadows{true};  // the characters' shadows (Tuning::bodyShadows)  // soft (mask, reduce, blur) or hard (stencil darken)
    std::atomic<bool> addDiscard{true};   // additive fragments adding nothing are discarded
    std::atomic<bool> skyImage{true};     // the sky / far scenery as a background image (PS2's zFog)
    // v167: the texture pack (Panda Venom, PCSX2 format, sdmc:/switch/celosia_nx/textures/pandavenom/): on/off
    // (hot); the files found (-1 not looked yet), textures replaced so far and their GPU megabytes (menu).
    std::atomic<bool> texturePack{false};
    // v168: draw distance 0 = 1x (PS2), 1 = 1.25x, 2 = 1.5x (models' level of detail and drop distance), hot.
    std::atomic<int> drawDistance{0};
    // v169: dynamic resolution: 0 off, else the least render scale in % (60 / 70 / 80 by preset); the scale in use.
    std::atomic<int> drsMinPercent{0};
    std::atomic<uint32_t> drsPercentNow{100};
    // v170: the camera's motion blur (cameraBlur's screen blurs; the scenes' own blurs stay), hot; the cheats
    // turned on (bit i: sotc_cheats's cheat i), hot.
    std::atomic<bool> motionBlur{true};
    std::atomic<uint32_t> cheats{0};
    std::atomic<bool> gpuBoost{false}; // v173s: GPU 460.8 MHz (LSFG / HD textures / PS3 quality preset)
    std::atomic<bool> swapFaceButtons{false}; // v173r: the game sees A<->B and X<->Y swapped (menu "swapbuttons")
    std::atomic<bool> boost4{false}; // v170: the 4-core boost (menu; native_renderer threads::setBoost4 each frame)
    std::atomic<int> texturePackFiles{-1};
    std::atomic<uint32_t> texturePackLoaded{0}, texturePackMB{0};
    // Published by the backend: the last GPU frame time (us), presentation rate (centi-FPS) and
    // the render size in use.
    std::atomic<uint32_t> gpuUs{0}, fpsCenti{0}, renderWidthNow{1280}, renderHeightNow{720};
    // Save states: the menu asks (slot 1..3), the game thread answers at its next safe point.
    std::atomic<int> saveRequest{0}, loadRequest{0};
    std::atomic<int> stateResult{0}; // 0 idle, 1 saved, 2 loaded, negative: failure code
    std::atomic<bool> menuOpen{false};
    // v172 photo mode (sotc_native.cpp photoFrame): 0 off, 1 entering (the HUD hidden, the frames captured), 2 frozen
    // (the game thread waits); photoShot: A pressed (save the picture); photoResult: photos saved (> 0) or the failure
    // (-1 no picture: frame generation on, -2 the SD) for the menu's message after it.
    std::atomic<int> photoMode{0}, photoShot{0}, photoResult{0};
    // v173 the free camera: the pad as held while frozen (sticks -1..1, HidNpadButton bits); X asks for a reset.
    std::atomic<float> photoLX{0.0f}, photoLY{0.0f}, photoRX{0.0f}, photoRY{0.0f};
    std::atomic<uint64_t> photoButtons{0};
    std::atomic<int> photoReset{0};
};

inline Settings &settings() { static Settings value; return value; }

// 16:9 width for a render height (multiple of 4, as the 4 x 4 shadow reduction wants).
inline uint32_t widthFor(uint32_t height) { return ((height * 16u / 9u) + 3u) & ~3u; }

}
