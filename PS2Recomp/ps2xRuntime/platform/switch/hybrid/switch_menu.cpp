// The in-game menu (Minus + Plus together): performance and graphics settings changed while the
// game runs, save states, and an optional frame-rate overlay. Input comes from pollSwitchInput
// (main thread); the menu is drawn into the native frame by the game thread at the frame's end
// (celosia_menu_frame, from sotc_native's frame end) as 2D screen draws over everything else.
// Settings persist in sdmc:/switch/celosia-native-boot/menu.cfg.
#include "live_settings.hpp"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include <switch.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
#include "../../vita/raylib/default_font.inc"

using celosia::live::settings;

constexpr uint64_t MenuId = 0xC0DE000000000000ull;
constexpr uint64_t FontTexture = MenuId | 0xF0F0u;
constexpr const char *ConfigPath = "sdmc:/switch/celosia-native-boot/menu.cfg";

struct Item {
    const char *label;
    std::vector<const char *> values; // empty: an action
    int value = 0;
    const char *help = "";
};
struct Tab {
    const char *name;
    std::vector<Item> items;
};


std::mutex lock;
bool open = false, loaded = false;
int tab = 0, row = 0;
std::vector<Tab> tabs;
std::string status;
uint64_t statusUntil = 0;
// Overlay: 0 off, 1 FPS, 2 FPS + GPU + resolution.
int overlay = 0;

uint64_t nowNs() { return armTicksToNs(armGetSystemTick()); }

void build()
{
    tabs = {
        {"Rendimiento", {
            {"Limite de FPS", {"60", "30"}, 0, "30: cada imagen se muestra dos refrescos (ritmo estable)."},
            {"Contador en pantalla", {"No", "FPS", "FPS + GPU"}, 0, "Arriba a la izquierda, con el menu cerrado."},
            {"Descartar luz aditiva vacia", {"Si", "No"}, 0, "Mismo color final; ahorra GPU en destellos."},
        }},
        {"Graficos", {
            {"Bloom del cielo", {"Si", "No"}, 0, "El resplandor del cielo y la distancia (PS2)."},
            {"Sombras", {"Suaves (PS2)", "Duras", "No"}, 0, "Suaves: como la PS2 (cuesta mas GPU)."},
            {"Sombras de personajes", {"Si", "No"}, 0, "Wander, Agro y los colosos sobre el suelo."},
            {"Cielo y niebla de fondo", {"Si", "No"}, 0, "La imagen de fondo de la PS2 tras la niebla."},
        }},
        {"Estados", {
            {"Ranura", {"1", "2", "3"}, 0, "Posiciones rapidas: duran mientras el juego siga abierto."},
            {"Guardar estado", {}, 0, "Guarda todo el juego en la SD (no toca las partidas de la tarjeta)."},
            {"Cargar estado", {}, 0, "Vuelve al estado guardado en esta ranura."},
        }},
    };
}

int &value(int t, int i) { return tabs[t].items[i].value; }

void apply()
{
    auto &s = settings();
    s.swapInterval = value(0, 0) == 1 ? 2 : 1;
    overlay = value(0, 1);
    s.addDiscard = value(0, 2) == 0;
    s.bloom = value(1, 0) == 0;
    s.shadows = value(1, 1) != 2;
    s.softShadows = value(1, 1) == 0;
    s.bodyShadows = value(1, 2) == 0;
    s.skyImage = value(1, 3) == 0;
}

extern "C" void celosia_states_install();

// v97: the save-state hook goes in here, the first thing either entry (the input poll or the
// frame) runs; v94-v96 installed it in celosia_menu_frame after the input poll had loaded.
void load()
{
    celosia_states_install();
    build();
    if (FILE *f = std::fopen(ConfigPath, "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            int t = 0, i = 0, v = 0;
            if (std::sscanf(line, "%d.%d=%d", &t, &i, &v) == 3 && t >= 0 && t < int(tabs.size()) && i >= 0 &&
                i < int(tabs[t].items.size()) && !tabs[t].items[i].values.empty())
                tabs[t].items[i].value = std::clamp(v, 0, int(tabs[t].items[i].values.size()) - 1);
        }
        std::fclose(f);
    }
    apply();
    loaded = true;
}

void save()
{
    if (FILE *f = std::fopen(ConfigPath, "w")) {
        for (int t = 0; t < int(tabs.size()); ++t)
            for (int i = 0; i < int(tabs[t].items.size()); ++i)
                if (!tabs[t].items[i].values.empty())
                    std::fprintf(f, "%d.%d=%d\n", t, i, tabs[t].items[i].value);
        std::fclose(f);
    }
}

void say(const std::string &text)
{
    status = text;
    statusUntil = nowNs() + 4000000000ull;
}

void activate()
{
    Item &item = tabs[tab].items[row];
    if (tab == 2 && item.values.empty()) {
        const int slot = value(2, 0) + 1;
        if (row == 1) {
            settings().stateResult = 0;
            settings().saveRequest = slot;
            say("Guardando estado " + std::to_string(slot) + "...");
        } else {
            settings().stateResult = 0;
            settings().loadRequest = slot;
            say("Cargando estado " + std::to_string(slot) + "...");
        }
    }
}

void change(int delta)
{
    Item &item = tabs[tab].items[row];
    if (item.values.empty())
        return;
    const int n = int(item.values.size());
    item.value = (item.value + delta + n) % n;
    apply();
}

// ---- input --------------------------------------------------------------------------------
uint64_t previous = 0;
uint64_t repeatAt = 0;
bool suppress = false; // after the combo / closing: the game sees nothing until all is released
uint64_t minusAt = 0, plusAt = 0, minusTapUntil = 0, plusTapUntil = 0;
constexpr uint64_t HoldNs = 90000000ull, TapNs = 120000000ull;

void navigate(uint64_t pressed, uint64_t held, uint64_t now)
{
    constexpr uint64_t Dirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
    uint64_t act = pressed;
    if (held & Dirs) {
        if (pressed & Dirs)
            repeatAt = now + 400000000ull;
        else if (now >= repeatAt) {
            act |= held & Dirs;
            repeatAt = now + 120000000ull;
        }
    }
    const int items = int(tabs[tab].items.size());
    if (act & HidNpadButton_L) { tab = (tab + int(tabs.size()) - 1) % int(tabs.size()); row = 0; }
    if (act & HidNpadButton_R) { tab = (tab + 1) % int(tabs.size()); row = 0; }
    if (act & HidNpadButton_AnyUp) row = (row + items - 1) % items;
    if (act & HidNpadButton_AnyDown) row = (row + 1) % items;
    if (act & HidNpadButton_AnyLeft) change(-1);
    if (act & HidNpadButton_AnyRight) change(1);
    if (act & HidNpadButton_A) {
        if (tabs[tab].items[row].values.empty()) activate();
        else change(1);
    }
    if (act & HidNpadButton_B) { open = false; save(); suppress = true; }
}
}

// Called by pollSwitchInput with this poll's buttons and sticks; leaves what the game may see.
void celosiaMenuInput(uint64_t &buttons, std::array<float, 4> &sticks)
{
    std::lock_guard guard(lock);
    if (!loaded)
        load();
    const uint64_t now = nowNs();
    const uint64_t pressed = buttons & ~previous;
    previous = buttons;
    const bool combo = (buttons & HidNpadButton_Minus) && (buttons & HidNpadButton_Plus) &&
                       ((pressed & HidNpadButton_Minus) || (pressed & HidNpadButton_Plus));
    if (combo) {
        open = !open;
        if (!open) save();
        suppress = true;
        minusAt = plusAt = minusTapUntil = plusTapUntil = 0;
    } else if (open) {
        navigate(pressed, buttons, now);
    }
    settings().menuOpen = open;
    if (open || suppress) {
        if (suppress && !open && !buttons)
            suppress = false;
        buttons = 0;
        sticks.fill(0);
        return;
    }
    // A lone Minus / Plus reaches the game after a short hold (or as a short tap once released),
    // so the first button of the combo never opens the game's own map or menu.
    auto gate = [&](uint64_t bit, uint64_t &at, uint64_t &tapUntil) {
        if (buttons & bit) {
            if (!at) at = now;
            if (now - at < HoldNs) buttons &= ~bit;
        } else {
            if (at && now - at < HoldNs) tapUntil = now + TapNs;
            at = 0;
            if (now < tapUntil) buttons |= bit;
        }
    };
    gate(HidNpadButton_Minus, minusAt, minusTapUntil);
    gate(HidNpadButton_Plus, plusAt, plusTapUntil);
}

// ---- drawing ------------------------------------------------------------------------------
namespace {
struct Glyph { int x, y, w; };
std::array<Glyph, 224> glyphs{};
bool glyphsReady = false;
std::vector<native_renderer::Vertex> rects, text;

void prepareFont()
{
    if (!glyphsReady) {
        int line = 0, x = 1, test = 1;
        for (unsigned i = 0; i < glyphs.size(); ++i) {
            const int w = kDefaultFontWidths[i];
            glyphs[i] = {x, 1 + line * 11, w};
            test += w + 1;
            if (test >= 128) { ++line; x = 2 + w; test = x; glyphs[i] = {1, 1 + line * 11, w}; }
            else x = test;
        }
        glyphsReady = true;
    }
    if (!native_renderer::hasTexture(FontTexture)) {
        std::vector<uint8_t> rgba(128 * 128 * 4);
        for (unsigned i = 0; i < 128 * 128; ++i) {
            const bool on = (kDefaultFontBits[i / 32] & (1u << (i % 32))) != 0;
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = on ? 0x80 : 0;
        }
        native_renderer::uploadTexture(FontTexture, 128, 128, rgba.data(), 1);
    }
}

uint8_t half(uint8_t v) { return uint8_t((unsigned(v) * 128 + 127) / 255); }

void quad(std::vector<native_renderer::Vertex> &out, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
          uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool textured)
{
    const float xs[4] = {x, x + w, x + w, x}, ys[4] = {y, y, y + h, y + h};
    const float us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    for (int corner : {0, 1, 2, 0, 2, 3}) {
        native_renderer::Vertex v{};
        v.x = xs[corner] / 640.0f - 1.0f;
        v.y = 1.0f - ys[corner] / 360.0f;
        v.u = us[corner];
        v.v = vs[corner];
        v.r = textured ? half(r) : r;
        v.g = textured ? half(g) : g;
        v.b = textured ? half(b) : b;
        v.a = half(a);
        out.push_back(v);
    }
}

void rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    quad(rects, x, y, w, h, 0, 0, 1, 1, r, g, b, a, false);
}

float measure(const char *s, float scale)
{
    float w = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        const unsigned c = *p >= 32 ? *p : '?';
        w += (kDefaultFontWidths[c - 32] + 1) * scale;
    }
    return w;
}

void print(const char *s, float x, float y, float scale, uint8_t r = 240, uint8_t g = 240, uint8_t b = 240)
{
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        const unsigned c = *p >= 32 ? *p : '?';
        const Glyph &gl = glyphs[c - 32];
        if (c != ' ')
            quad(text, x, y, gl.w * scale, 10 * scale, gl.x / 128.0f, gl.y / 128.0f, (gl.x + gl.w) / 128.0f, (gl.y + 10) / 128.0f,
                 r, g, b, 255, true);
        x += (gl.w + 1) * scale;
    }
}

void submit()
{
    constexpr float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!rects.empty()) {
        native_renderer::uploadMesh(MenuId | 1u, rects.data(), uint32_t(rects.size()), true);
        native_renderer::DrawState state;
        state.screen = true; state.noDepthWrite = true; state.clamp = 3; state.order = 0x7FFF0000u;
        native_renderer::draw(MenuId | 1u, identity, state);
    }
    if (!text.empty()) {
        native_renderer::uploadMesh(MenuId | 2u, text.data(), uint32_t(text.size()), true);
        native_renderer::DrawState state;
        state.screen = true; state.noDepthWrite = true; state.clamp = 3; state.order = 0x7FFF0001u;
        state.texture = FontTexture;
        native_renderer::draw(MenuId | 2u, identity, state);
    }
    rects.clear();
    text.clear();
}

void drawOverlay()
{
    auto &s = settings();
    char line[96];
    const double fps = s.fpsCenti / 100.0, gpu = s.gpuUs / 1000.0;
    if (overlay == 1) std::snprintf(line, sizeof(line), "%.1f FPS", fps);
    else std::snprintf(line, sizeof(line), "%.1f FPS  GPU %.1f ms  %ux%u", fps, gpu, unsigned(s.renderWidthNow), unsigned(s.renderHeightNow));
    rect(8, 8, measure(line, 2.0f) + 16, 30, 0, 0, 0, 160);
    print(line, 16, 13, 2.0f, 255, 255, 120);
}

void drawMenu()
{
    const float x0 = 60, y0 = 50, w = 760, rowH = 40;
    const Tab &t = tabs[tab];
    const float h = 150 + rowH * float(t.items.size()) + 70;
    rect(x0, y0, w, h, 12, 14, 20, 225);
    rect(x0, y0, w, 52, 35, 55, 85, 245);
    print("COLOSIA NX", x0 + 20, y0 + 11, 3.0f, 255, 255, 255);
    // Tabs
    float tx = x0 + 20;
    for (int i = 0; i < int(tabs.size()); ++i) {
        const float tw = measure(tabs[i].name, 2.0f) + 24;
        if (i == tab) rect(tx, y0 + 64, tw, 32, 70, 110, 165, 255);
        else rect(tx, y0 + 64, tw, 32, 30, 34, 44, 255);
        print(tabs[i].name, tx + 12, y0 + 70, 2.0f, i == tab ? 255 : 170, i == tab ? 255 : 170, i == tab ? 255 : 170);
        tx += tw + 8;
    }
    // Items
    float y = y0 + 112;
    for (int i = 0; i < int(t.items.size()); ++i) {
        const Item &item = t.items[i];
        if (i == row) rect(x0 + 10, y - 4, w - 20, rowH - 4, 50, 80, 125, 255);
        print(item.label, x0 + 24, y + 4, 2.0f);
        if (!item.values.empty()) {
            char v[64];
            std::snprintf(v, sizeof(v), "<  %s  >", item.values[item.value]);
            print(v, x0 + w - 24 - measure(v, 2.0f), y + 4, 2.0f, 255, 225, 120);
        } else {
            print("[A]", x0 + w - 24 - measure("[A]", 2.0f), y + 4, 2.0f, 140, 220, 255);
        }
        y += rowH;
    }
    // Help, status and footer
    print(t.items[row].help, x0 + 24, y + 6, 1.5f, 170, 190, 210);
    if (nowNs() < statusUntil)
        print(status.c_str(), x0 + 24, y + 30, 2.0f, 140, 255, 160);
    rect(x0, y0 + h - 34, w, 34, 25, 30, 40, 245);
    print("L/R pestana   Arriba/Abajo opcion   Izq/Der cambiar   A aceptar   B cerrar", x0 + 16, y0 + h - 26, 1.5f, 200, 200, 200);
}
}

// The game thread, once per native frame before endFrame.

extern "C" void celosia_menu_frame()
{
    std::lock_guard guard(lock);
    if (!loaded)
        load();
    // Save state results from the game side.
    const int result = settings().stateResult.exchange(0);
    if (result == 1) say("Estado guardado.");
    else if (result == 2) say("Estado cargado.");
    else if (result == -2) say("Ranura vacia.");
    else if (result == -3) say("El estado no es valido para esta version.");
    else if (result == -5) say("Ahora no se puede (el juego esta leyendo el disco).");
    else if (result == -11) say("No se pudo escribir en la SD.");
    else if (result < 0) say("No se pudo (codigo " + std::to_string(result) + ").");
    // Game-side switches, written on this (the game) thread and only when the menu changed them,
    // so the SOTC_NATIVE_* environment switches still hold until then.
    {
        static int lastShadows = -1, lastBody = -1;
        const int shadows = settings().shadows ? 1 : 0, body = settings().bodyShadows ? 1 : 0;
        if (lastShadows >= 0 && shadows != lastShadows) native_renderer::tuning().shadows = shadows != 0;
        if (lastBody >= 0 && body != lastBody) native_renderer::tuning().bodyShadows = body != 0;
        if (lastShadows < 0 && !shadows) native_renderer::tuning().shadows = false;
        if (lastBody < 0 && !body) native_renderer::tuning().bodyShadows = false;
        lastShadows = shadows;
        lastBody = body;
    }
    if (!open && overlay == 0)
        return;
    prepareFont();
    if (open) drawMenu();
    else drawOverlay();
    submit();
}
