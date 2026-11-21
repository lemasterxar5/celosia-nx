// The in-game menu (Minus + Plus together): performance and graphics settings changed while the
// game runs, save states, and an optional frame-rate overlay. Input comes from pollSwitchInput
// (main thread); the menu is drawn into the native frame by the game thread at the frame's end
// (celosia_menu_frame, from sotc_native's frame end) as 2D screen draws over everything else.
// Settings persist in sdmc:/switch/celosia_nx/menu.cfg as key=value lines.
// v149: every text in English, Spanish and Brazilian Portuguese (the console's language by default),
// sliders for ordered choices, and red notes for choices that need more of the display.
#include "live_settings.hpp"
#include "runtime/native_renderer.h"
#include "runtime/native_tuning.h"
#include <switch.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <vector>

namespace {
#include "../../vita/raylib/default_font.inc"
#include "title_font.inc" // v171

using celosia::live::settings;

constexpr uint64_t MenuId = 0xC0DE000000000000ull;
constexpr uint64_t FontTexture = MenuId | 0xF0F0u;
constexpr uint64_t TitleTexture = MenuId | 0xF0F1u; // v171: the title in Cinzel
constexpr const char *ConfigPath = "sdmc:/switch/celosia_nx/menu.cfg";

// A text in the menu's three languages: English, Spanish, Brazilian Portuguese (UTF-8).
struct Text {
    const char *en = "", *es = "", *pt = "";
};
int language = 0; // the language shown: 0 English, 1 Spanish, 2 Portuguese
const char *tr(const Text &t) { return language == 1 ? t.es : language == 2 ? t.pt : t.en; }
Text same(const char *t) { return {t, t, t}; }
const Text Yes{"On", "Sí", "Sim"}, No{"Off", "No", "Não"};

enum class Kind { Choice, Slider, Action };
struct Item {
    const char *key;
    Text label;
    Kind kind;
    std::vector<Text> values; // empty: an action
    int value = 0;
    Text help;
    int warnFrom = -1; // values from this index on show `warn` in red
    Text warn;
    std::vector<Text> valueHelp; // v168: help of each value (shown in place of `help` when not empty)
    bool advanced = false; // v171: in the Graphics tab's "Advanced graphics" group (shown when it is open)
    int lock = 0; // v170: 1 needs the LSFG folder, 2 needs the texture pack's folder, 3 a 4-core process (else fixed at its first value)
};
// v170: what the SD card holds, looked at once when the menu loads (the game does not see them change).
bool lsfgFound = false, packFound = false, fourCores = false;
bool advancedOpen = false; // v171
// v171: an on/off option (its two values On and Off, either order): drawn as a switch, changed only with A.
bool isToggle(const Item &i)
{
    if (i.kind != Kind::Choice || i.values.size() != 2) return false;
    const char *a = i.values[0].en, *b = i.values[1].en;
    return (!std::strcmp(a, "On") && !std::strcmp(b, "Off")) || (!std::strcmp(a, "Off") && !std::strcmp(b, "On"));
}
bool toggleOn(const Item &i) { return !std::strcmp(i.values[i.value].en, "On"); }
bool locked(const Item &i) { return (i.lock == 1 && !lsfgFound) || (i.lock == 2 && !packFound) || (i.lock == 3 && !fourCores); }
void detectFolders()
{
    fourCores = native_renderer::threads::core3Allowed(); // v170: the forwarder's core mask includes core 3
    // v173m: the shader cache prepared as Cemu-nx's (Cache/Quality, Cache/Performance)
    for (const char *m : {"sdmc:/switch/celosia_nx/lsfg/Cache/Performance/manifest.bin", "sdmc:/switch/celosia_nx/lsfg/Cache/Quality/manifest.bin"})
        if (FILE *f = std::fopen(m, "r")) { std::fclose(f); lsfgFound = true; }
    if (DIR *d = opendir("sdmc:/switch/celosia_nx/textures/pandavenom")) {
        while (const dirent *e = readdir(d))
            if (e->d_name[0] != '.') { packFound = true; break; }
        closedir(d);
    }
}
struct Tab {
    Text name;
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
        {{"Graphics", "Gráficos", "Gráficos"}, {
            // v168: presets. Choosing one sets the graphics options below at once; changing any of them after
            // shows "Custom". Frame generation, the counter and the menu's own options are left as they are.
            {"preset", {"Preset", "Perfil", "Perfil"}, Kind::Choice,
             {{"Custom", "Personalizado", "Personalizado"}, {"Performance", "Rendimiento", "Desempenho"},
              {"Balanced", "Balanceado", "Equilibrado"}, {"Quality", "Calidad", "Qualidade"},
              {"PS3 quality", "Calidad PS3", "Qualidade PS3"}}, 0,
             {"", "", ""}, -1, {"", "", ""},
             {{"Your own settings.", "Tus propios ajustes.", "Seus próprios ajustes."},
              {"PS2 resolution (720p docked), extras off. Helps reach 60 FPS.",
               "Resolución de PS2 (720p en dock), extras desactivados. Ayuda a alcanzar 60 FPS.",
               "Resolução do PS2 (720p no dock), extras desligados. Ajuda a alcançar 60 FPS."},
              {"540p (900p docked), hard shadows, sliders at their minimum. For 45 FPS.",
               "540p (900p en dock), sombras duras, deslizadores al mínimo. Para 45 FPS.",
               "540p (900p no dock), sombras duras, controles no mínimo. Para 45 FPS."},
              {"Maximum resolution, the other options at their middle, detail distance 1x. For 30 FPS.",
               "Resolución máxima, el resto de opciones a la mitad, distancia de detalle 1x. Para 30 FPS.",
               "Resolução máxima, as outras opções no meio, distância de detalhe 1x. Para 30 FPS."},
              {"Everything at its maximum + Panda Venom HD textures, for 30 FPS. May need overclocking in some scenes.",
               "Todo al máximo + texturas HD Panda Venom, para 30 FPS. Podría requerir overclock en ciertas escenas.",
               "Tudo no máximo + texturas HD Panda Venom, para 30 FPS. Pode exigir overclock em certas cenas."}}},
            // v171: the group's entry (A opens / closes it); every option below it but frame generation's two.
            {"advanced", {"Advanced graphics", "Gráficos avanzados", "Gráficos avançados"}, Kind::Action, {}, 0,
             {"Shows or hides every graphics option (the presets set them for you).",
              "Muestra u oculta todas las opciones de gráficos (los perfiles las ajustan por ti).",
              "Mostra ou oculta todas as opções de gráficos (os perfis as ajustam para você)."}},
            {"fps", {"Frame cap", "Límite de FPS", "Limite de FPS"}, Kind::Slider, {same("30"), same("45"), same("60")}, 0,
             {"Real frames per second. 45: one, one and two refreshes in turn.",
              "Fotogramas reales por segundo. 45: uno, uno y dos refrescos por turnos.",
              "Quadros reais por segundo. 45: um, um e dois refrescos alternados."}},
            {"reshandheld", {"Resolution (handheld)", "Resolución (portátil)", "Resolução (portátil)"}, Kind::Slider,
             {{"Original", "Original", "Original"}, same("540p"), same("720p")}, 2,
             {"Used in handheld mode; changes at once. Original: the PS2's 448 lines.",
              "Se usa en modo portátil; cambia al momento. Original: las 448 líneas de la PS2.",
              "Usada no modo portátil; muda na hora. Original: as 448 linhas do PS2."}},
            {"resdocked", {"Resolution (docked)", "Resolución (dock)", "Resolução (dock)"}, Kind::Slider,
             {same("720p"), same("900p"), same("1080p")}, 2,
             {"Used in the dock; changes at once. Above 720p it is drawn sharper and scaled to the output.",
              "Se usa en el dock; cambia al momento. Por encima de 720p se dibuja más nítido y se escala a la salida.",
              "Usada no dock; muda na hora. Acima de 720p é desenhada mais nítida e escalada para a saída."}},
            // v170: its own mode, independent of the presets (user): each mode's least scale. Key "drsmode" (v169's
            // on/off "drs" is not read: an old setting starts off).
            {"drsmode", {"Dynamic resolution", "Resolución dinámica", "Resolução dinâmica"}, Kind::Choice,
             {No, {"Quality", "Calidad", "Qualidade"}, {"Balanced", "Balanceado", "Equilibrado"}, {"Performance", "Rendimiento", "Desempenho"}}, 0,
             {"Lowers the resolution only while the GPU is above 87 %. Down to: Quality 80 %, Balanced 70 %, Performance 60 %.",
              "Baja la resolución solo mientras la GPU pasa del 87 %. Hasta: Calidad 80 %, Balanceado 70 %, Rendimiento 60 %.",
              "Baixa a resolução só enquanto a GPU passa de 87 %. Até: Qualidade 80 %, Equilibrado 70 %, Desempenho 60 %."}},
            {"upscale", {"Upscaling filter", "Filtro de escalado", "Filtro de escala"}, Kind::Choice,
             {{"Bilinear", "Bilineal", "Bilinear"}, {"Bicubic (sharper)", "Bicúbico (más nítido)", "Bicúbico (mais nítido)"}}, 0,
             {"How a picture drawn below the output's resolution is enlarged.",
              "Cómo se amplía la imagen cuando se dibuja por debajo de la resolución de salida.",
              "Como a imagem é ampliada quando é desenhada abaixo da resolução de saída."}},
            {"aa", {"Anti-aliasing", "Antialiasing", "Antisserrilhamento"}, Kind::Slider,
             {No, same("FXAA"), same("MSAA 2x"), same("MSAA 4x"), same("MSAA 4x + FXAA")}, 0,
             {"Smooths jagged edges. MSAA costs more GPU (most at high resolutions); FXAA is light.",
              "Suaviza los bordes dentados. MSAA cuesta más GPU (sobre todo a alta resolución); FXAA es ligero.",
              "Suaviza as bordas serrilhadas. MSAA custa mais GPU (sobretudo em alta resolução); FXAA é leve."}},
            {"sharpen", {"Sharpening", "Nitidez", "Nitidez"}, Kind::Slider,
             {No, {"Low", "Baja", "Baixa"}, {"Medium", "Media", "Média"}, {"High", "Alta", "Alta"}}, 0,
             {"Adaptive sharpening of the final picture, without halos.",
              "Realce adaptativo de la imagen final, sin halos.",
              "Realce adaptativo da imagem final, sem halos."}},
            {"framegen", {"Frame generation (LSFG)", "Generación de fotogramas (LSFG)", "Geração de quadros (LSFG)"}, Kind::Slider,
             {No, same("60 FPS (30 x2)"), same("90 FPS (45 x2)")}, 0, // v173s: 120 (60 x2) removed (a saved 3 loads as 2)
             {"Lossless Scaling frame generation with your own copy. When the game falls below the rate, doubles the real FPS.",
              "La generación de fotogramas de Lossless Scaling con tu copia. Si el juego baja de la tasa, duplica los FPS reales.",
              "A geração de quadros do Lossless Scaling com a sua cópia. Se o jogo cair abaixo da taxa, dobra os FPS reais."},
             2, {"Only works if the display is set up for a 90 Hz output.",
                 "Solo funciona si la pantalla está configurada para una salida de 90 Hz.",
                 "Só funciona se a tela estiver configurada para uma saída de 90 Hz."}},
            {"fgquality", {"Frame generation quality", "Calidad de la generación", "Qualidade da geração"}, Kind::Slider,
             {{"Performance", "Rendimiento", "Desempenho"}, {"Balanced", "Equilibrada", "Equilibrada"}, {"Quality", "Calidad", "Qualidade"}}, 0,
             {"Resolution of the motion estimate: higher looks better and costs more GPU.",
              "Resolución con la que se estima el movimiento: más alta se ve mejor y cuesta más GPU.",
              "Resolução da estimativa de movimento: mais alta fica melhor e custa mais GPU."}},
            {"fgperf", {"Frame generation performance mode", "Modo rendimiento de la generación", "Modo desempenho da geração"}, Kind::Choice,
             {Yes, No}, 0,
             {"Lossless Scaling's lighter network (as Cemu-nx, on by default): less GPU, a little less precise.",
              "La red más ligera de Lossless Scaling (como Cemu-nx, activado por defecto): menos GPU, algo menos precisa.",
              "A rede mais leve do Lossless Scaling (como o Cemu-nx, ativado por padrão): menos GPU, um pouco menos precisa."}},
            {"aniso", {"Anisotropic filtering", "Filtrado anisotrópico", "Filtragem anisotrópica"}, Kind::Slider,
             {No, same("2x"), same("4x"), same("8x"), same("16x")}, 0,
             {"Sharper ground and walls at an angle. Off: as on PS2.",
              "Suelo y paredes más nítidos en ángulo. No: como en PS2.",
              "Chão e paredes mais nítidos em ângulo. Não: como no PS2."}},
            {"texdetail", {"Texture detail", "Detalle de texturas", "Detalhe das texturas"}, Kind::Slider,
             {same("PS2"), {"High", "Alto", "Alto"}, {"Maximum", "Máximo", "Máximo"}}, 0,
             {"Sharper textures in the distance (finer mipmaps). May shimmer a little more.",
              "Texturas más nítidas a lo lejos (mipmaps más finos). Puede parpadear algo más.",
              "Texturas mais nítidas ao longe (mipmaps mais finos). Pode cintilar um pouco mais."}},
            {"drawdist", {"Detail distance", "Distancia de detalle", "Distância de detalhe"}, Kind::Slider,
             {same("1x"), same("1.5x"), same("2x"), same("3x")}, 0, // v173s: was 1x / 1.25x / 1.5x
             {"Models keep their full detail farther away (textures a little too). 1x: as on PS2. More GPU and CPU.",
              "Los modelos mantienen su detalle a más distancia (las texturas un poco también). 1x: como en PS2. Más GPU y CPU.",
              "Os modelos mantêm o detalhe a mais distância (as texturas um pouco também). 1x: como no PS2. Mais GPU e CPU."}},
            {"texpack", {"HD textures (Panda Venom)", "Texturas HD (Panda Venom)", "Texturas HD (Panda Venom)"}, Kind::Choice, {No, Yes}, 0,
             {"The Panda Venom pack (sd:/switch/celosia_nx/textures/pandavenom). Uses more memory.",
              "El pack Panda Venom (sd:/switch/celosia_nx/textures/pandavenom). Usa más memoria.",
              "O pacote Panda Venom (sd:/switch/celosia_nx/textures/pandavenom). Usa mais memória."}},
            {"bloom", {"Sky bloom", "Bloom del cielo", "Bloom do céu"}, Kind::Choice, {Yes, No}, 0,
             {"The glow of the sky and the distance (PS2).", "El resplandor del cielo y la distancia (PS2).",
              "O brilho do céu e da distância (PS2)."}},
            {"mblur", {"Motion blur", "Desenfoque de movimiento", "Desfoque de movimento"}, Kind::Choice, {Yes, No}, 0,
             {"The camera's blur when it turns fast (PS2). The scenes' own effects stay.",
              "El desenfoque de la cámara al girar rápido (PS2). Los efectos de las escenas se mantienen.",
              "O desfoque da câmera ao girar rápido (PS2). Os efeitos das cenas continuam."}},
            {"shadows", {"Shadows", "Sombras", "Sombras"}, Kind::Choice,
             {{"Soft (PS2)", "Suaves (PS2)", "Suaves (PS2)"}, {"Hard", "Duras", "Duras"}, No}, 0,
             {"Soft: as on PS2 (costs more GPU).", "Suaves: como en PS2 (cuesta más GPU).", "Suaves: como no PS2 (custa mais GPU)."}},
            {"bodyshadows", {"Character shadows", "Sombras de personajes", "Sombras de personagens"}, Kind::Choice, {Yes, No}, 0,
             {"Wander, Agro and the colossi on the ground.", "Wander, Agro y los colosos sobre el suelo.",
              "Wander, Agro e os colossos no chão."}},
            {"sky", {"Sky and background fog", "Cielo y niebla de fondo", "Céu e névoa de fundo"}, Kind::Choice, {Yes, No}, 0,
             {"The PS2's background image behind the fog.", "La imagen de fondo de la PS2 tras la niebla.",
              "A imagem de fundo do PS2 atrás da névoa."}},
            {"deband", {"Reduce colour banding", "Reducir bandas de color", "Reduzir faixas de cor"}, Kind::Choice, {No, Yes}, 0,
             {"A very fine dither: smooth gradients in the sky and the fog.",
              "Un tramado muy fino: degradados suaves en el cielo y la niebla.",
              "Um pontilhado muito fino: degradês suaves no céu e na névoa."}},
            {"overlay", {"On-screen counter", "Contador en pantalla", "Contador na tela"}, Kind::Choice,
             {No, same("FPS"), same("FPS + GPU")}, 0,
             {"Top left, while the menu is closed.", "Arriba a la izquierda, con el menú cerrado.",
              "No canto superior esquerdo, com o menu fechado."}},
            {"adddiscard", {"Skip empty additive light", "Descartar luz aditiva vacía", "Descartar luz aditiva vazia"}, Kind::Choice,
             {Yes, No}, 0,
             {"Same final colour; saves GPU on glows.", "Mismo color final; ahorra GPU en destellos.",
              "Mesma cor final; economiza GPU em brilhos."}},
        }},
        // v170: cheats (the NTSC-U disc's known codes, data writes only: sotc_cheats.cpp), each on its own, hot.
        {{"Cheats", "Trucos", "Truques"}, {
            {"cheat_health", {"Infinite health", "Vida infinita", "Vida infinita"}, Kind::Choice, {No, Yes}, 0,
             {"Wander's health stays full.",
              "La vida de Wander se mantiene al máximo.",
              "A vida de Wander fica sempre cheia."}},
            {"cheat_grip", {"Infinite grip", "Agarre infinito", "Agarre infinito"}, Kind::Choice, {No, Yes}, 0,
             {"The grip gauge never runs out.",
              "El indicador de agarre nunca se agota.",
              "O medidor de agarre nunca acaba."}},
            {"cheat_items", {"All items", "Todos los objetos", "Todos os itens"}, Kind::Choice, {No, Yes}, 0,
             {"Every item unlocked and equipped (masks, cloaks, arrows, maps).",
              "Todos los objetos desbloqueados y equipados (máscaras, capas, flechas, mapas).",
              "Todos os itens desbloqueados e equipados (máscaras, capas, flechas, mapas)."}},
            {"cheat_c15", {"Story skip to colossus 15", "Saltar historia al coloso 15", "Pular história até o colosso 15"}, Kind::Choice, {No, Yes}, 0,
             {"With this on, press R1 + Down in the game: the story jumps ahead, colossus 15 becomes the next one (the earlier ones count as beaten).",
              "Con esto activado, pulsa R1 + abajo en el juego: la historia avanza y el coloso 15 pasa a ser el siguiente (los anteriores cuentan como derrotados).",
              "Com isto ativado, aperte R1 + baixo no jogo: a história avança e o colosso 15 passa a ser o próximo (os anteriores contam como derrotados)."}},
            {"cheat_c16", {"Story skip to colossus 16", "Saltar historia al coloso 16", "Pular história até o colosso 16"}, Kind::Choice, {No, Yes}, 0,
             {"With this on, press R1 + Left in the game: the story jumps ahead, colossus 16 becomes the next one (the earlier ones count as beaten).",
              "Con esto activado, pulsa R1 + izquierda en el juego: la historia avanza y el coloso 16 pasa a ser el siguiente (los anteriores cuentan como derrotados).",
              "Com isto ativado, aperte R1 + esquerda no jogo: a história avança e o colosso 16 passa a ser o próximo (os anteriores contam como derrotados)."}},
        }},
        {{"States", "Estados", "Estados"}, {
            {"slot", {"Slot", "Ranura", "Slot"}, Kind::Choice, {same("1"), same("2"), same("3")}, 0,
             {"Kept on the SD card.", "Se guardan en la SD.", "Ficam no cartão SD."}},
            {"save", {"Save state", "Guardar estado", "Salvar estado"}, Kind::Action, {}, 0,
             {"Saves the whole game to the SD (memory card saves untouched).",
              "Guarda todo el juego en la SD (no toca las partidas de la tarjeta).",
              "Salva o jogo inteiro no SD (não mexe nos saves do cartão)."}},
            {"load", {"Load state", "Cargar estado", "Carregar estado"}, Kind::Action, {}, 0,
             {"Returns to the state saved in this slot.", "Vuelve al estado guardado en esta ranura.",
              "Volta ao estado salvo neste slot."}},
        }},
        {{"Menu", "Menú", "Menu"}, {
            // v172: photo mode (sotc_native.cpp photoFrame).
            {"photo", {"Photo mode", "Modo foto", "Modo foto"}, Kind::Action, {}, 0,
             {"Freezes the game without the HUD, free camera: left stick moves, right stick looks, A up, B down, ZR faster, ZL slower, L / R tilt, X resets, Y photo (SD: /switch/celosia_nx/Modo foto), + back.",
              "Congela el juego sin el HUD, cámara libre: stick izquierdo mueve, derecho mira, A sube, B baja, ZR rápido, ZL lento, L / R inclinan, X reinicia, Y foto (SD: /switch/celosia_nx/Modo foto), + vuelve.",
              "Congela o jogo sem o HUD, câmera livre: analógico esquerdo move, direito olha, A sobe, B desce, ZR rápido, ZL lento, L / R inclinam, X reinicia, Y foto (SD: /switch/celosia_nx/Modo foto), + volta."}},
            {"lang", {"Language", "Idioma", "Idioma"}, Kind::Choice,
             {{"Console", "Consola", "Console"}, same("English"), same("Español"), same("Português (BR)")}, 0,
             {"Console: the Switch's language (English unless Spanish or Portuguese).",
              "Consola: el idioma de la Switch (inglés si no es español ni portugués).",
              "Console: o idioma do Switch (inglês se não for espanhol nem português)."}},
            // v173r: the face buttons as the Switch's layout (the game only; the menu and photo mode keep theirs).
            {"swapbuttons", {"Switch button layout", "Distribución de botones de Switch", "Layout de botões do Switch"}, Kind::Choice, {No, Yes}, 0,
             {"Swaps A with B and X with Y in the game (confirm on A, as on the Switch). The menu and photo mode keep their buttons.",
              "Intercambia A con B y X con Y en el juego (confirmar con A, como en la Switch). El menú y el modo foto conservan sus botones.",
              "Troca A com B e X com Y no jogo (confirmar com A, como no Switch). O menu e o modo foto mantêm seus botões."}},
            // v170: the 4-core boost (manual; only with a forwarder made with 4 cores): the helper threads on core 3.
            {"boost4", {"4-core boost", "Boost de 4 núcleos", "Boost de 4 núcleos"}, Kind::Choice, {No, Yes}, 0,
             {"Moves the helper threads (models, textures, disc, audio, logs, presentation) to the fourth core.",
              "Mueve los hilos auxiliares (modelos, texturas, disco, audio, registros, presentación) al cuarto núcleo.",
              "Move as threads auxiliares (modelos, texturas, disco, áudio, registros, apresentação) para o quarto núcleo."},
             0,
             {"Only usable if the forwarder was created with 4 cores.",
              "Solo se puede usar si el forwarder se creó con 4 núcleos.",
              "Só pode ser usado se o forwarder foi criado com 4 núcleos."}},
            {"savecfg", {"Save settings", "Guardar configuración", "Salvar configurações"}, Kind::Action, {}, 0,
             {"Writes every setting to the SD now (also done when the menu closes); kept across restarts.",
              "Guarda ya todos los ajustes en la SD (también al cerrar el menú); se mantienen al reiniciar.",
              "Grava agora todos os ajustes no SD (também ao fechar o menu); ficam após reiniciar."}},
        }},
    };
}

Item *find(const char *key)
{
    for (auto &t : tabs)
        for (auto &i : t.items)
            if (!std::strcmp(i.key, key)) return &i;
    return nullptr;
}
int value(const char *key)
{
    const Item *i = find(key);
    return i ? i->value : 0;
}

// The console's language as the menu's: Spanish (Spain, Latin America), Portuguese (both), else English.
int systemLanguage()
{
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        u64 code = 0;
        SetLanguage lang = SetLanguage_ENUS;
        if (R_SUCCEEDED(setInitialize())) {
            if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang))) {
                if (lang == SetLanguage_ES || lang == SetLanguage_ES419) cached = 1;
                else if (lang == SetLanguage_PT || lang == SetLanguage_PTBR) cached = 2;
            }
            setExit();
        }
    }
    return cached;
}

void apply()
{
    auto &s = settings();
    static const int caps[3] = {30, 45, 60};
    s.fpsCap = caps[std::clamp(value("fps"), 0, 2)];
    s.swapInterval = s.fpsCap.load() == 30 ? 2 : 1;
    overlay = value("overlay");
    s.addDiscard = value("adddiscard") == 0;
    s.anisotropy = 1 << std::clamp(value("aniso"), 0, 4);
    static const int handheld[3] = {448, 540, 720}, docked[3] = {720, 900, 1080};
    s.handheldHeight = handheld[std::clamp(value("reshandheld"), 0, 2)];
    s.dockedHeight = docked[std::clamp(value("resdocked"), 0, 2)];
    s.upscaleFilter = std::clamp(value("upscale"), 0, 1);
    s.antiAliasing = std::clamp(value("aa"), 0, 4);
    s.sharpen = std::clamp(value("sharpen"), 0, 3);
    s.textureDetail = std::clamp(value("texdetail"), 0, 2);
    // v168: the texture pack follows the setting read at start only (the user's rule: a restart applies it).
    static const bool packAtStart = value("texpack") == 1;
    s.texturePack = packAtStart;
    s.drawDistance = std::clamp(value("drawdist"), 0, 3);
    {
        static const int least[4] = {0, 80, 70, 60}; // v170: off, Quality, Balanced, Performance (not the preset's)
        s.drsMinPercent = least[std::clamp(value("drsmode"), 0, 3)];
    }
    s.deband = value("deband") == 1;
    s.frameGen = std::clamp(value("framegen"), 0, 2);
    s.frameGenQuality = std::clamp(value("fgquality"), 0, 2);
    s.frameGenPerformance = value("fgperf") == 0; // v173m
    s.bloom = value("bloom") == 0;
    s.shadows = value("shadows") != 2;
    s.softShadows = value("shadows") == 0;
    s.bodyShadows = value("bodyshadows") == 0;
    s.skyImage = value("sky") == 0;
    s.motionBlur = value("mblur") == 0;
    s.boost4 = value("boost4") == 1 && fourCores;
    s.swapFaceButtons = value("swapbuttons") == 1;
    // v173s (the user's request): the GPU at 460.8 MHz while the frame generation, the HD textures (as loaded)
    // or the PS3 quality preset is on, each enough on its own; 384 MHz otherwise (main.cpp's watchdog).
    s.gpuBoost = s.frameGen.load() > 0 || s.texturePack.load() || value("preset") == 4;
    {
        static const char *const cheatKeys[] = {"cheat_health", "cheat_grip", "cheat_items", "cheat_c15", "cheat_c16"};
        uint32_t mask = 0;
        for (int i = 0; i < 5; ++i)
            mask |= value(cheatKeys[i]) == 1 ? 1u << i : 0u;
        s.cheats = mask;
    }
    const int lang = value("lang");
    language = lang == 0 ? systemLanguage() : lang - 1;
}

extern "C" void celosia_states_install();

// v97: the save-state hook goes in here, the first thing either entry (the input poll or the
// frame) runs; v94-v96 installed it in celosia_menu_frame after the input poll had loaded.
void load()
{
    celosia_states_install();
    detectFolders();
    build();
    for (const char *k : {"framegen", "fgquality", "fgperf"}) if (Item *i = find(k)) i->lock = 1;
    if (Item *i = find("texpack")) i->lock = 2;
    if (Item *i = find("boost4")) i->lock = 3;
    for (auto &t : tabs) {
        if (std::strcmp(t.name.en, "Graphics")) continue;
        std::stable_partition(t.items.begin(), t.items.end(),
                              [](const Item &i) { return std::strcmp(i.key, "framegen") && std::strcmp(i.key, "fgquality") && std::strcmp(i.key, "fgperf"); });
        for (auto &i : t.items)
            i.advanced = std::strcmp(i.key, "preset") && std::strcmp(i.key, "advanced") && std::strcmp(i.key, "framegen") &&
                         std::strcmp(i.key, "fgquality") && std::strcmp(i.key, "fgperf");
    }
    if (Item *i = find("preset"); i && !packFound && i->values.size() == 5) i->values.pop_back(); // no "PS3 quality"
    std::fprintf(stderr, "[menu] lsfg folder %s, texture pack %s, 4 cores %s\n", lsfgFound ? "found" : "missing", packFound ? "found" : "missing",
                 fourCores ? "yes" : "no");
    if (FILE *f = std::fopen(ConfigPath, "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            char key[32];
            int v = 0;
            // key=value (v149); the older "tab.item=value" lines are left out (the defaults hold).
            // v170a: keys with digits and '_' too (cheat_health, boost4: v170 read none of them).
            if (std::sscanf(line, "%31[a-z0-9_]=%d", key, &v) == 2)
                if (Item *i = find(key); i && !i->values.empty())
                    i->value = std::clamp(v, 0, int(i->values.size()) - 1);
        }
        std::fclose(f);
    }
    for (auto &t : tabs) for (auto &i : t.items) if (locked(i)) i.value = 0; // off without its files
    apply();
    loaded = true;
}

bool save()
{
    FILE *f = std::fopen(ConfigPath, "w");
    if (!f)
        return false;
    bool ok = true;
    for (const auto &t : tabs)
        for (const auto &i : t.items)
            if (!i.values.empty())
                ok = std::fprintf(f, "%s=%d\n", i.key, i.value) > 0 && ok;
    return std::fclose(f) == 0 && ok;
}

void say(const std::string &text)
{
    status = text;
    statusUntil = nowNs() + 4000000000ull;
}

bool suppress = false; // after the combo / closing / photo mode: the game sees nothing until all is released
void activate()
{
    const Item &item = tabs[tab].items[row];
    if (item.kind != Kind::Action)
        return;
    if (!std::strcmp(item.key, "advanced")) { advancedOpen = !advancedOpen; return; } // v171
    if (!std::strcmp(item.key, "photo")) { open = false; save(); suppress = true; settings().photoResult = 0; settings().photoMode = 1; return; } // v172
    if (!std::strcmp(item.key, "savecfg")) {
        say(save() ? tr({"Settings saved.", "Configuración guardada.", "Configurações salvas."})
                   : tr({"Could not write to the SD card.", "No se pudo escribir en la SD.", "Não foi possível gravar no cartão SD."}));
        return;
    }
    const int slot = value("slot") + 1;
    settings().stateResult = 0;
    if (!std::strcmp(item.key, "save")) {
        settings().saveRequest = slot;
        say(std::string(tr({"Saving state ", "Guardando estado ", "Salvando estado "})) + std::to_string(slot) + "...");
    } else {
        settings().loadRequest = slot;
        say(std::string(tr({"Loading state ", "Cargando estado ", "Carregando estado "})) + std::to_string(slot) + "...");
    }
}

// v168: the presets' values (key, value per preset 1..4); keys not listed are left alone.
struct PresetValue { const char *key; int v[4]; };
const PresetValue Presets[] = {
    //               Rendimiento Balanceado Calidad CalidadPS3   (fps: 0 = 30, 1 = 45, 2 = 60)
    {"fps",         {2, 1, 0, 0}},
    {"reshandheld", {0, 1, 2, 2}},
    {"resdocked",   {0, 1, 2, 2}},
    {"upscale",     {1, 1, 1, 1}},
    // v171: Calidad = each slider at its middle (MSAA 2x, medium sharpening, 4x, high, 1x); Calidad PS3 = all max.
    {"aa",          {0, 0, 2, 4}},
    {"sharpen",     {0, 0, 2, 3}},
    {"aniso",       {0, 0, 2, 4}},
    {"texdetail",   {0, 0, 1, 2}},
    {"drawdist",    {0, 0, 0, 3}}, // v173s: PS3 quality = all max (3x)
    {"texpack",     {0, 0, 0, 1}},
    {"bloom",       {0, 0, 0, 0}},
    {"shadows",     {1, 1, 0, 0}},
    {"bodyshadows", {0, 0, 0, 0}},
    {"sky",         {0, 0, 0, 0}},
    {"deband",      {0, 1, 1, 1}},
};
void applyPreset(int preset)
{
    if (preset < 1 || preset > 4) return;
    for (const auto &p : Presets)
        if (Item *i = find(p.key); i && !i->values.empty() && !locked(*i))
            i->value = std::clamp(p.v[preset - 1], 0, int(i->values.size()) - 1);
}
bool presetKey(const char *key)
{
    for (const auto &p : Presets)
        if (!std::strcmp(p.key, key)) return true;
    return false;
}

void change(int delta)
{
    Item &item = tabs[tab].items[row];
    if (item.values.empty() || locked(item))
        return;
    const int n = int(item.values.size());
    // Sliders stop at their ends; lists wrap around.
    if (item.kind == Kind::Slider) item.value = std::clamp(item.value + delta, 0, n - 1);
    else item.value = (item.value + delta + n) % n;
    if (!std::strcmp(item.key, "preset")) applyPreset(item.value);
    else if (presetKey(item.key)) if (Item *p = find("preset")) p->value = 0;
    apply();
}

// ---- input --------------------------------------------------------------------------------
uint64_t previous = 0;
uint64_t repeatAt = 0;
// (suppress, declared above activate: after the combo / closing: the game sees nothing until all is released
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
    // v171: the rows of a closed "Advanced graphics" group are skipped.
    const auto shownRow = [&](int r) { return !tabs[tab].items[r].advanced || advancedOpen; };
    if (act & HidNpadButton_AnyUp) do row = (row + items - 1) % items; while (!shownRow(row));
    if (act & HidNpadButton_AnyDown) do row = (row + 1) % items; while (!shownRow(row));
    const bool toggle = isToggle(tabs[tab].items[row]);
    if ((act & HidNpadButton_AnyLeft) && !toggle) change(-1);
    if ((act & HidNpadButton_AnyRight) && !toggle) change(1);
    if (act & HidNpadButton_A) {
        if (tabs[tab].items[row].kind == Kind::Action) activate();
        else change(1);
    }
    if (act & HidNpadButton_B) { open = false; save(); suppress = true; }
}
}

uint64_t celosiaMenuBetween = 0;
// Called by pollSwitchInput with this poll's buttons and sticks; leaves what the game may see.
void celosiaMenuInput(uint64_t &buttons, std::array<float, 4> &sticks)
{
    std::lock_guard guard(lock);
    if (!loaded)
        load();
    const uint64_t now = nowNs();
    // v169: presses that began and ended between two polls (HID's history) count as pressed now.
    const uint64_t pressed = (buttons | celosiaMenuBetween) & ~previous;
    previous = buttons;
    const bool combo = (buttons & HidNpadButton_Minus) && (buttons & HidNpadButton_Plus) &&
                       ((pressed & HidNpadButton_Minus) || (pressed & HidNpadButton_Plus));
    // v172 photo mode: A a photo, B (or the menu combo) back to the game; the game sees nothing meanwhile.
    if (settings().photoMode.load(std::memory_order_acquire) != 0 && !open) {
        // v173 free camera: the pad goes to the camera (sotc_native photoFrame) once the menu's press is released.
        settings().photoLX = suppress ? 0.0f : sticks[0]; settings().photoLY = suppress ? 0.0f : sticks[1];
        settings().photoRX = suppress ? 0.0f : sticks[2]; settings().photoRY = suppress ? 0.0f : sticks[3];
        settings().photoButtons = suppress ? 0u : buttons;
        if (!suppress) {
            if (pressed & HidNpadButton_Y) settings().photoShot = 1;
            if (pressed & HidNpadButton_X) settings().photoReset = 1;
            if ((pressed & HidNpadButton_Plus) || combo) { settings().photoMode = 0; suppress = true; }
        } else if (!buttons) suppress = false;
        settings().menuOpen = false;
        buttons = 0;
        sticks.fill(0);
        return;
    }
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
std::vector<native_renderer::Vertex> rects, text, title; // v171: title (TitleTexture)

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
    if (!native_renderer::hasTexture(TitleTexture)) {
        std::vector<uint8_t> rgba(size_t(kTitleW) * kTitleH * 4);
        for (size_t i = 0; i < size_t(kTitleW) * kTitleH; ++i) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = uint8_t((unsigned(kTitleBits[i]) * 128 + 127) / 255);
        }
        native_renderer::uploadTexture(TitleTexture, kTitleW, kTitleH, rgba.data(), 1);
    }
}

// v149: the font holds Latin-1 (32..255); the texts are UTF-8. A two-byte sequence of U+0080..U+00FF
// gives its Latin-1 code, anything else outside ASCII a '?'.
unsigned nextChar(const unsigned char *&p)
{
    const unsigned c = *p++;
    if (c < 0x80) return c >= 32 ? c : '?';
    if ((c & 0xE0u) == 0xC0u && (*p & 0xC0u) == 0x80u) {
        const unsigned code = ((c & 0x1Fu) << 6) | (*p++ & 0x3Fu);
        return code >= 160 && code <= 255 ? code : '?';
    }
    while ((*p & 0xC0u) == 0x80u) ++p;
    return '?';
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
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p;)
        w += (kDefaultFontWidths[nextChar(p) - 32] + 1) * scale;
    return w;
}

void print(const char *s, float x, float y, float scale, uint8_t r = 240, uint8_t g = 240, uint8_t b = 240)
{
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p;) {
        const unsigned c = nextChar(p);
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
    if (!title.empty()) {
        native_renderer::uploadMesh(MenuId | 3u, title.data(), uint32_t(title.size()), true);
        native_renderer::DrawState state;
        state.screen = true; state.noDepthWrite = true; state.clamp = 3; state.order = 0x7FFF0002u;
        state.texture = TitleTexture;
        native_renderer::draw(MenuId | 3u, identity, state);
    }
    rects.clear();
    text.clear();
    title.clear();
}

void drawOverlay()
{
    auto &s = settings();
    char line[96];
    const double fps = s.fpsCenti / 100.0, gpu = s.gpuUs / 1000.0;
    // v155: with frame generation, the images shown per second and the real ones in brackets.
    static const Text realWord{"real", "reales", "reais"};
    char rate[48];
    if (s.frameGen.load(std::memory_order_relaxed) > 0 && s.frameGenShown.load(std::memory_order_relaxed) > 0)
        std::snprintf(rate, sizeof(rate), "%.1f FPS (%.1f %s)", s.frameGenShown / 100.0, s.frameGenReal / 100.0, tr(realWord));
    else std::snprintf(rate, sizeof(rate), "%.1f FPS", fps);
    if (overlay == 1) std::snprintf(line, sizeof(line), "%s", rate);
    else std::snprintf(line, sizeof(line), "%s  GPU %.1f ms  %ux%u", rate, gpu, unsigned(s.renderWidthNow), unsigned(s.renderHeightNow));
    rect(8, 8, measure(line, 2.0f) + 16, 30, 0, 0, 0, 160);
    print(line, 16, 13, 2.0f, 255, 255, 120);
}

// A slider: a track with a stop per value, the chosen one filled, its name to the right.
void drawSlider(const Item &item, float right, float y, bool selected)
{
    const int n = int(item.values.size());
    const char *name = tr(item.values[item.value]);
    const float nameW = std::max(measure(name, 2.0f), 64.0f);
    const float trackW = 220, x1 = right - nameW - 24, x0 = x1 - trackW;
    rect(x0, y + 12, trackW, 6, 60, 60, 60, 255);
    rect(x0, y + 12, trackW * float(item.value) / float(std::max(n - 1, 1)), 6, 200, 200, 200, 255);
    for (int i = 0; i < n; ++i) {
        const float cx = x0 + trackW * float(i) / float(std::max(n - 1, 1));
        const bool on = i == item.value;
        rect(cx - (on ? 7 : 4), y + 15 - (on ? 9 : 5), on ? 14 : 8, on ? 18 : 10, on ? 255 : 130, on ? 255 : 130,
             on ? 255 : 130, 255);
    }
    if (locked(item)) print(name, right - measure(name, 2.0f), y + 4, 2.0f, 120, 120, 130);
    else print(name, right - measure(name, 2.0f), y + 4, 2.0f, selected ? 255 : 235, selected ? 255 : 235, selected ? 255 : 235);
}
// v171: a small switch (a short track, a small dot at its right end when on, at its left when off).
void drawToggle(const Item &item, float right, float y, bool selected)
{
    const bool on = toggleOn(item), dim = locked(item);
    const float w = 34, h = 16, x = right - w, top = y + 7, dot = 10;
    const uint8_t track = dim ? 45 : on ? 185 : 70;
    rect(x, top, w, h, track, track, track, 255);
    const uint8_t knob = dim ? 90 : on ? 255 : (selected ? 170 : 140);
    rect(on ? x + w - dot - 3 : x + 3, top + (h - dot) / 2, dot, dot, knob, knob, knob, 255);
}

void drawMenu()
{
    const float x0 = 60, y0 = 50, w = 900, rowH = 40;
    const Tab &t = tabs[tab];
    const Item &current = t.items[row];
    // v168: the texture pack (its own option, or the PS3 quality preset that sets it) applies after a restart:
    // red while the chosen value differs from the one the game started with.
    static const Text restartNote{"Requires restarting the game to apply the change.",
        "Requiere reiniciar el juego para aplicar el cambio.", "Requer reiniciar o jogo para aplicar a mudança."};
    const bool packChanged = (value("texpack") == 1) != settings().texturePack.load(std::memory_order_relaxed);
    const bool restart = packChanged && (!std::strcmp(current.key, "texpack") || (!std::strcmp(current.key, "preset") && current.value == 4));
    static const Text lsfgNote{"Only usable if you have the folder with the Lossless Scaling files (/switch/celosia_nx/lsfg).",
        "Solo se puede usar si tienes la carpeta con los archivos de Lossless (/switch/celosia_nx/lsfg).",
        "Só pode ser usado se você tiver a pasta com os arquivos do Lossless (/switch/celosia_nx/lsfg)."},
        packNote{"Needs the Panda Venom texture pack in /switch/celosia_nx/textures/pandavenom to work.",
        "Necesitas el paquete de texturas Panda Venom en /switch/celosia_nx/textures/pandavenom para que funcione.",
        "Precisa do pacote de texturas Panda Venom em /switch/celosia_nx/textures/pandavenom para funcionar."};
    const bool lockedNow = locked(current);
    const bool warning = lockedNow || restart || (current.warnFrom >= 0 && current.value >= current.warnFrom);
    // v150: at most VisibleRows rows shown; the list scrolls to keep the selected one in view.
    constexpr int VisibleRows = 10;
    std::vector<int> vis; // v171: the rows shown (a closed "Advanced graphics" group hides its options)
    for (int i = 0; i < int(t.items.size()); ++i)
        if (!t.items[i].advanced || advancedOpen) vis.push_back(i);
    const int vrow = int(std::find(vis.begin(), vis.end(), row) - vis.begin());
    const int count = int(vis.size()), shown = std::min(count, VisibleRows);
    static int first = 0;
    first = std::clamp(first, std::max(0, vrow - shown + 1), std::min(vrow, std::max(0, count - shown)));
    const float h = 150 + rowH * float(shown) + 70 + (warning ? 24 : 0);
    rect(x0, y0, w, h, 8, 8, 8, 235);
    rect(x0, y0, w, 52, 38, 38, 38, 245);
    // v171: the title in Cinzel (title_font.inc, drawn at half its size).
    quad(title, x0 + 18, y0 + 12, kTitleTextW * 0.5f, kTitleTextH * 0.5f, 0.0f, float(kTitleTextTop) / kTitleH,
         float(kTitleTextW) / kTitleW, float(kTitleTextTop + kTitleTextH) / kTitleH, 255, 255, 255, 255, true);
    {   // v151: the mode in use (console or ReverseNX-RT) and the scene's size.
        static const Text docked{"Docked", "Dock", "Dock"}, handheld{"Handheld", "Portátil", "Portátil"};
        const auto &live = celosia::live::settings();
        char mode[64];
        std::snprintf(mode, sizeof(mode), "%s  %ux%u", tr(live.docked.load(std::memory_order_relaxed) ? docked : handheld),
            unsigned(live.renderWidthNow.load(std::memory_order_relaxed)), unsigned(live.renderHeightNow.load(std::memory_order_relaxed)));
        print(mode, x0 + w - 20 - measure(mode, 2.0f), y0 + 17, 2.0f, 200, 200, 200);
    }
    // Tabs
    float tx = x0 + 20;
    for (int i = 0; i < int(tabs.size()); ++i) {
        const char *name = tr(tabs[i].name);
        const float tw = measure(name, 2.0f) + 24;
        if (i == tab) rect(tx, y0 + 64, tw, 32, 105, 105, 105, 255);
        else rect(tx, y0 + 64, tw, 32, 28, 28, 28, 255);
        print(name, tx + 12, y0 + 70, 2.0f, i == tab ? 255 : 170, i == tab ? 255 : 170, i == tab ? 255 : 170);
        tx += tw + 8;
    }
    // Items
    float y = y0 + 112;
    if (first > 0) print("^", x0 + w - 30, y0 + 100, 1.5f, 200, 200, 200);
    for (int k = first; k < first + shown; ++k) {
        const int i = vis[k];
        const Item &item = t.items[i];
        if (i == row) rect(x0 + 10, y - 4, w - 20, rowH - 4, 64, 64, 64, 255);
        print(tr(item.label), x0 + 24 + (item.advanced ? 18 : 0), y + 4, 2.0f); // v171: the group's options indented
        const float right = x0 + w - 24;
        if (item.kind == Kind::Slider) {
            drawSlider(item, right, y, i == row);
        } else if (isToggle(item)) {
            drawToggle(item, right, y, i == row);
        } else if (item.kind == Kind::Choice) {
            char v[96];
            std::snprintf(v, sizeof(v), locked(item) ? "%s" : "<  %s  >", tr(item.values[item.value]));
            if (locked(item)) print(v, right - measure(v, 2.0f), y + 4, 2.0f, 120, 120, 130);
            else print(v, right - measure(v, 2.0f), y + 4, 2.0f, 235, 235, 235);
        } else if (!std::strcmp(item.key, "advanced")) {
            static const Text showT{"Show  [A]", "Mostrar  [A]", "Mostrar  [A]"}, hideT{"Hide  [A]", "Ocultar  [A]", "Ocultar  [A]"};
            const char *v = tr(advancedOpen ? hideT : showT);
            print(v, right - measure(v, 2.0f), y + 4, 2.0f, 210, 210, 210);
        } else {
            print("[A]", right - measure("[A]", 2.0f), y + 4, 2.0f, 210, 210, 210);
        }
        y += rowH;
    }
    if (first + shown < count) print("v", x0 + w - 30, y - 10, 1.5f, 200, 200, 200);
    // Help, the red note of the chosen value, status and footer
    print(tr(current.valueHelp.empty() ? current.help : current.valueHelp[std::clamp(current.value, 0, int(current.valueHelp.size()) - 1)]),
          x0 + 24, y + 6, 1.5f, 180, 180, 180);
    if (warning) {
        y += 24;
        print(tr(lockedNow ? (current.lock == 1 ? lsfgNote : current.lock == 2 ? packNote : current.warn) : restart ? restartNote : current.warn), x0 + 24, y + 6, 1.5f, 255, 70, 70);
    }
    if (nowNs() < statusUntil)
        print(status.c_str(), x0 + 24, y + 30, 2.0f, 140, 255, 160);
    else if (std::strncmp(current.key, "f", 1) == 0 && std::strcmp(current.key, "fps") != 0 && settings().frameGenState.load(std::memory_order_relaxed)) {
        static const Text missing{"LSFG shaders not found on the SD card (/switch/celosia_nx/lsfg).",
            "No se encuentran los shaders de LSFG en la SD (/switch/celosia_nx/lsfg).",
            "Shaders do LSFG não encontrados no cartão SD (/switch/celosia_nx/lsfg)."},
            memory{"Not enough GPU memory for frame generation here.",
            "No hay memoria de GPU para la generación de fotogramas aquí.",
            "Sem memória de GPU para a geração de quadros aqui."};
        print(tr(settings().frameGenState.load() == 1 ? missing : memory), x0 + 24, y + 30, 1.5f, 255, 220, 120);
    }
    else if (std::strcmp(current.key, "aa") == 0 && settings().msaaFailed.load(std::memory_order_relaxed)) {
        static const Text failed{"Not enough GPU memory for this MSAA here: drawn without it.",
            "No hay memoria de GPU para este MSAA aquí: se dibuja sin él.",
            "Sem memória de GPU para este MSAA aqui: desenhado sem ele."};
        print(tr(failed), x0 + 24, y + 30, 1.5f, 255, 220, 120);
    }
    rect(x0, y0 + h - 34, w, 34, 24, 24, 24, 245);
    print(tr({"L/R tab   Up/Down option   Left/Right change   A accept   B close",
              "L/R pestaña   Arriba/Abajo opción   Izq/Der cambiar   A aceptar   B cerrar",
              "L/R aba   Cima/Baixo opção   Esq/Dir mudar   A aceitar   B fechar"}),
          x0 + 16, y0 + h - 26, 1.5f, 200, 200, 200);
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
    if (result == 1) say(tr({"State saved.", "Estado guardado.", "Estado salvo."}));
    else if (result == 2) say(tr({"State loaded.", "Estado cargado.", "Estado carregado."}));
    else if (result == -2) say(tr({"Empty slot.", "Ranura vacía.", "Slot vazio."}));
    else if (result == -3) say(tr({"The state is not valid for this version.", "El estado no es válido para esta versión.",
                                   "O estado não é válido para esta versão."}));
    else if (result == -5) say(tr({"Not now (the game is reading the disc).", "Ahora no se puede (el juego está leyendo el disco).",
                                   "Agora não dá (o jogo está lendo o disco)."}));
    else if (result == -11) say(tr({"Could not write to the SD card.", "No se pudo escribir en la SD.",
                                    "Não foi possível gravar no cartão SD."}));
    else if (result < 0) say(std::string(tr({"Failed (code ", "No se pudo (código ", "Falhou (código "})) + std::to_string(result) + ").");
    // v172 photo mode: what it saved, once back in the game.
    if (settings().photoMode.load(std::memory_order_acquire) == 0) {
        const int photos = settings().photoResult.exchange(0);
        if (photos > 0) say(std::string(tr({"Photos saved: ", "Fotos guardadas: ", "Fotos salvas: "})) + std::to_string(photos) + " (/switch/celosia_nx/Modo foto)");
        else if (photos == -1) say(tr({"No photo: not available with frame generation (LSFG).", "Sin foto: no disponible con la generación de fotogramas (LSFG).",
                                       "Sem foto: indisponível com a geração de quadros (LSFG)."}));
        else if (photos == -2) say(tr({"Could not write to the SD card.", "No se pudo escribir en la SD.", "Não foi possível gravar no cartão SD."}));
    }
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
    if (!open && (overlay == 0 || settings().photoMode.load(std::memory_order_relaxed) != 0)) // v172: nothing over a photo
        return;
    prepareFont();
    if (open) drawMenu();
    else drawOverlay();
    submit();
}
