// Diagnostic for the actual Celosia deko3d backend. No game data or game runtime.
#include "native_backend.h"
#include <switch.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>

int main()
{
    const Result romfs = romfsInit();
    if (R_FAILED(romfs)) return 1;
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/celosia-probe", 0777);
    FILE *log = std::fopen("sdmc:/switch/celosia-probe/probe.log", "w");
    if (!log) { romfsExit(); return 2; }
    std::fprintf(log, "Celosia deko3d diagnostic; NOT THE GAME; build %s %s\n", __DATE__, __TIME__);
    std::fprintf(log, "scene=1280x720; no stock-performance claim\n");
    std::fflush(log);
    int result = 0;
    try
    {
        std::unique_ptr<native_renderer::detail::Backend> backend(
            native_renderer::detail::createDeko3dBackend());
        if (!backend) throw std::runtime_error("deko3d backend creation failed");
        using native_renderer::Vertex;
        const Vertex triangle[] = {
            {-0.75f, -0.65f, 0.5f, 0, 0, 1, 0, 0, 128, 0, 0, 128},
            { 0.75f, -0.65f, 0.5f, 0, 0, 1, 1, 0, 0, 128, 0, 128},
            { 0.0f,   0.65f, 0.5f, 0, 0, 1, 0.5f, 1, 0, 0, 128, 128}
        };
        backend->uploadMesh(1, triangle, 3, false);
        const uint8_t checker[] = {
            255,255,255,255, 32,32,32,255,
            32,32,32,255, 255,255,255,255
        };
        backend->uploadTexture(2, 2, 2, checker, 1);
        const float clip[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        native_renderer::DrawState state;
        state.texture = 2;
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        unsigned frames = 0;
        while (appletMainLoop() && frames < 600)
        {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            backend->beginFrame();
            backend->draw(1, clip, state);
            if (frames == 119) native_renderer::detail::requestDeko3dCapture(*backend);
            backend->endFrame(1280, 720);
            ++frames;
            if (frames == 120)
            {
                std::vector<uint8_t> rgba;
                uint32_t width = 0, height = 0;
                if (!backend->latestFrame(rgba, width, height))
                    throw std::runtime_error("diagnostic capture unavailable");
                if (width != 1280 || height != 720 || rgba.size() != 1280u * 720u * 4u)
                    throw std::runtime_error("diagnostic capture has wrong scene size");
                const auto centre = (size_t(height / 2) * width + width / 2) * 4;
                if (rgba[centre] < 8 || rgba[centre + 1] < 8 || rgba[centre + 2] < 8)
                    throw std::runtime_error("diagnostic triangle is missing at centre");
                FILE *capture = std::fopen("sdmc:/switch/celosia-probe/frame.rgba", "wb");
                if (!capture) throw std::runtime_error("cannot save diagnostic capture");
                const bool saved = std::fwrite(rgba.data(), 1, rgba.size(), capture) == rgba.size();
                const int closed = std::fclose(capture);
                if (!saved || closed) throw std::runtime_error("incomplete diagnostic capture");
                uint64_t hash = 14695981039346656037ull;
                for (uint8_t byte : rgba) { hash ^= byte; hash *= 1099511628211ull; }
                std::fprintf(log, "capture=%ux%u bytes=%zu fnv1a64=%016llx\n",
                    width, height, rgba.size(), static_cast<unsigned long long>(hash));
                std::fflush(log);
            }
            if (frames == 1 || frames % 60 == 0)
            {
                std::fprintf(log, "submitted_frames=%u\n", frames);
                std::fflush(log);
            }
        }
        backend->finishFrames();
        std::fprintf(log, "diagnostic completed; frames=%u\n", frames);
    }
    catch (const std::exception &error)
    {
        std::fprintf(log, "FAILED: %s\n", error.what());
        result = 3;
    }
    std::fclose(log);
    romfsExit();
    return result;
}
