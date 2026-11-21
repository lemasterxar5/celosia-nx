// v167: texture packs in PCSX2's replacement format (see sotc_texpack.h).
//
// The name: <TEX0 hash>-<CLUT hash>-<bits>.dds, hex. bits = PSM | log2 TW << 6 | log2 TH << 10 | TCC << 14
// (TEXA's fields after it are 0 for palette textures). TEX0 hash: XXH3-64 (seed 0) of the texture as GS
// memory holds it: for a texture at least one GS block in each direction (PSMT8 16 x 16, PSMT4 32 x 16)
// the 256 bytes of each block, blocks row by row; for a smaller one (PSMT4 16 wide) the palette indices,
// one byte each, row by row. CLUT hash: XXH3-64 of the palette's 32-bit entries in index order (16 or
// 256). The block bytes come from the indices through the GS's column tables (PCSX2 GSTables.cpp,
// GPL-3.0+: the GS's own layout, tables of facts), so the source layout in guest memory (linear or
// pre-swizzled as PSMCT32) does not matter.
#include "sotc_texpack.h"

#define XXH_INLINE_ALL
#include "third_party/xxhash.h"
#include "runtime/native_renderer.h"
#include "../../PS2Recomp/ps2xRuntime/platform/switch/hybrid/live_settings.hpp"
#include "ThreadNaming.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>

namespace sotc_texpack
{
    namespace
    {
#if defined(__SWITCH__)
        constexpr const char *kRoot = "sdmc:/switch/celosia_nx/textures";
#else
        constexpr const char *kRoot = "textures";
#endif
        constexpr const char *kPack = "pandavenom";

        // The GS's byte (PSMT8) / nibble (PSMT4) of each texel within a block (PCSX2 columnTable8 / 4).
        constexpr uint8_t kColumn8[16][16] = {
            {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
            {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
            {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
            {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
            {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
            {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
            {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
            {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
            {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
            {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
            {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
            {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
            {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
            {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
            {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
            {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255},
        };
        constexpr uint16_t kColumn4[16][32] = {
            {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
            {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
            {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
            {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
            {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
            {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
            {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
            {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
            {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
            {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
            {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
            {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
            {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
            {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
            {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
            {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511},
        };

        struct Entry
        {
            uint64_t clut = 0;
            uint32_t bits = 0;
            std::string name;
        };
        std::vector<Entry> g_entries;                                  // handle = index + 1
        std::unordered_multimap<uint64_t, uint32_t> g_byTex;           // TEX0 hash -> entry index
        std::atomic<bool> g_scanned{false};
        std::atomic<uint64_t> g_finds{0}, g_hits{0}, g_mipHits{0}, g_missLogged{0};

        struct Job
        {
            uint64_t key = 0;
            uint32_t handle = 0, ps2W = 0, ps2H = 0, generation = 0;
            bool gridWanted = false;
        };
        struct Done
        {
            uint64_t key = 0;
            uint32_t generation = 0;
            std::vector<uint8_t> bc3, grid;
            native_renderer::CompressedTexture t;
        };
        std::mutex g_lock;
        std::condition_variable g_wake;
        std::deque<Job> g_jobs;
        std::deque<std::unique_ptr<Done>> g_done;
        bool g_scanAsked = false, g_threadStarted = false;
        std::atomic<uint32_t> g_generation{1};
        uint64_t g_requests = 0, g_uploadTries = 0, g_staleDrops = 0; // v168 diagnostics (game thread)
        std::atomic<uint64_t> g_bytesLoaded{0}, g_overBudget{0}, g_failed{0};

        // Game thread state.
        uint64_t g_salt = 0;
        bool g_wasOn = false;
        std::vector<uint64_t> g_uploadedKeys;
        uint64_t g_frames = 0;

        uint64_t budgetBytes()
        {
            static const uint64_t mb = []
            {
                const char *v = std::getenv("CELOSIA_TEXPACK_MB");
                return v ? static_cast<uint64_t>(std::max(64, std::atoi(v))) : 768ull;
            }();
            return mb << 20;
        }

        void scan()
        {
            mkdir(kRoot, 0777);
            const std::string dir = std::string(kRoot) + "/" + kPack;
            mkdir(dir.c_str(), 0777);
            int files = 0;
            if (DIR *d = opendir(dir.c_str()))
            {
                while (const dirent *e = readdir(d))
                {
                    const char *n = e->d_name;
                    unsigned long long tex = 0, clut = 0;
                    unsigned bits = 0;
                    int used = 0;
                    if (std::sscanf(n, "%llx-%llx-%x.dds%n", &tex, &clut, &bits, &used) != 3 || used <= 0 || n[used] != '\0')
                        continue;
                    g_byTex.emplace(static_cast<uint64_t>(tex), static_cast<uint32_t>(g_entries.size()));
                    g_entries.push_back({static_cast<uint64_t>(clut), bits, dir + "/" + n});
                    ++files;
                }
                closedir(d);
            }
            celosia::live::settings().texturePackFiles = files;
            std::fprintf(stderr, "[texpack] %s: %d textures\n", dir.c_str(), files);
            g_scanned.store(true, std::memory_order_release);
        }

        // ---- BC3 -----------------------------------------------------------------------------------
        inline void expand565(uint16_t c, int &r, int &g, int &b)
        {
            r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            r = (r << 3) | (r >> 2), g = (g << 2) | (g >> 4), b = (b << 3) | (b >> 2);
        }
        void decodeBlock(const uint8_t *s, uint8_t out[16][4])
        {
            uint8_t a[8];
            a[0] = s[0], a[1] = s[1];
            if (a[0] > a[1])
                for (int i = 0; i < 6; ++i)
                    a[2 + i] = static_cast<uint8_t>(((6 - i) * a[0] + (i + 1) * a[1]) / 7);
            else
            {
                for (int i = 0; i < 4; ++i)
                    a[2 + i] = static_cast<uint8_t>(((4 - i) * a[0] + (i + 1) * a[1]) / 5);
                a[6] = 0, a[7] = 255;
            }
            uint64_t ai = 0;
            for (int i = 0; i < 6; ++i)
                ai |= static_cast<uint64_t>(s[2 + i]) << (8 * i);
            const uint16_t c0 = static_cast<uint16_t>(s[8] | (s[9] << 8)), c1 = static_cast<uint16_t>(s[10] | (s[11] << 8));
            int p[4][3];
            expand565(c0, p[0][0], p[0][1], p[0][2]);
            expand565(c1, p[1][0], p[1][1], p[1][2]);
            for (int k = 0; k < 3; ++k)
                p[2][k] = (2 * p[0][k] + p[1][k]) / 3, p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
            const uint32_t ci = static_cast<uint32_t>(s[12] | (s[13] << 8) | (s[14] << 16) | (s[15] << 24));
            for (int i = 0; i < 16; ++i)
            {
                const int c = (ci >> (2 * i)) & 3;
                out[i][0] = static_cast<uint8_t>(p[c][0]), out[i][1] = static_cast<uint8_t>(p[c][1]), out[i][2] = static_cast<uint8_t>(p[c][2]);
                out[i][3] = a[(ai >> (3 * i)) & 7];
            }
        }
        inline uint16_t to565(int r, int g, int b)
        {
            return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
        }
        // Fast range fit (bounding box, inset), enough for the mip levels.
        void encodeBlock(const uint8_t in[16][4], uint8_t *d)
        {
            int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
            for (int i = 0; i < 16; ++i)
                for (int k = 0; k < 4; ++k)
                    lo[k] = std::min<int>(lo[k], in[i][k]), hi[k] = std::max<int>(hi[k], in[i][k]);
            // alpha
            d[0] = static_cast<uint8_t>(hi[3]), d[1] = static_cast<uint8_t>(lo[3]);
            uint64_t ai = 0;
            if (hi[3] > lo[3])
                for (int i = 0; i < 16; ++i)
                {
                    const int t = ((in[i][3] - lo[3]) * 7 + (hi[3] - lo[3]) / 2) / (hi[3] - lo[3]);
                    const int idx = t == 7 ? 0 : (t == 0 ? 1 : 8 - t);
                    ai |= static_cast<uint64_t>(idx) << (3 * i);
                }
            for (int i = 0; i < 6; ++i)
                d[2 + i] = static_cast<uint8_t>(ai >> (8 * i));
            // colour
            int a[3], b[3];
            for (int k = 0; k < 3; ++k)
            {
                const int inset = (hi[k] - lo[k]) >> 4;
                a[k] = hi[k] - inset, b[k] = lo[k] + inset;
            }
            const uint16_t c0 = to565(a[0], a[1], a[2]), c1 = to565(b[0], b[1], b[2]);
            d[8] = static_cast<uint8_t>(c0), d[9] = static_cast<uint8_t>(c0 >> 8), d[10] = static_cast<uint8_t>(c1), d[11] = static_cast<uint8_t>(c1 >> 8);
            uint32_t ci = 0;
            if (c0 != c1)
            {
                int p[4][3];
                expand565(c0, p[0][0], p[0][1], p[0][2]);
                expand565(c1, p[1][0], p[1][1], p[1][2]);
                for (int k = 0; k < 3; ++k)
                    p[2][k] = (2 * p[0][k] + p[1][k]) / 3, p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
                for (int i = 0; i < 16; ++i)
                {
                    int best = 0, bestD = 1 << 30;
                    for (int c = 0; c < 4; ++c)
                    {
                        const int dr = in[i][0] - p[c][0], dg = in[i][1] - p[c][1], db = in[i][2] - p[c][2];
                        const int dd = dr * dr + dg * dg + db * db;
                        if (dd < bestD)
                            bestD = dd, best = c;
                    }
                    ci |= static_cast<uint32_t>(best) << (2 * i);
                }
            }
            d[12] = static_cast<uint8_t>(ci), d[13] = static_cast<uint8_t>(ci >> 8), d[14] = static_cast<uint8_t>(ci >> 16), d[15] = static_cast<uint8_t>(ci >> 24);
        }
        void decodeImage(const uint8_t *bc3, uint32_t w, uint32_t h, std::vector<uint8_t> &rgba)
        {
            rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
            uint8_t px[16][4];
            for (uint32_t by = 0; by < (h + 3) / 4; ++by)
                for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx, bc3 += 16)
                {
                    decodeBlock(bc3, px);
                    for (uint32_t y = 0; y < 4; ++y)
                        for (uint32_t x = 0; x < 4; ++x)
                            if (by * 4 + y < h && bx * 4 + x < w)
                                std::memcpy(&rgba[((by * 4 + y) * w + bx * 4 + x) * 4u], px[y * 4 + x], 4);
                }
        }
        void encodeImage(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
        {
            uint8_t px[16][4];
            const size_t at = out.size();
            out.resize(at + static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 16u);
            uint8_t *d = out.data() + at;
            for (uint32_t by = 0; by < (h + 3) / 4; ++by)
                for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx, d += 16)
                {
                    for (uint32_t y = 0; y < 4; ++y)
                        for (uint32_t x = 0; x < 4; ++x)
                        {
                            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                            std::memcpy(px[y * 4 + x], &rgba[(static_cast<size_t>(sy) * w + sx) * 4u], 4);
                        }
                    encodeBlock(px, d);
                }
        }
        void halve(const std::vector<uint8_t> &in, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
        {
            const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            out.resize(static_cast<size_t>(nw) * nh * 4u);
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x)
                {
                    const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1), y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                    for (int k = 0; k < 4; ++k)
                        out[(static_cast<size_t>(y) * nw + x) * 4u + k] = static_cast<uint8_t>(
                            (in[(static_cast<size_t>(y0) * w + x0) * 4u + k] + in[(static_cast<size_t>(y0) * w + x1) * 4u + k] +
                             in[(static_cast<size_t>(y1) * w + x0) * 4u + k] + in[(static_cast<size_t>(y1) * w + x1) * 4u + k] + 2) / 4);
                }
        }

        bool readFile(const std::string &path, std::vector<uint8_t> &data)
        {
            FILE *f = std::fopen(path.c_str(), "rb");
            if (!f)
                return false;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            bool ok = size > 128 && size < (64l << 20);
            if (ok)
            {
                data.resize(static_cast<size_t>(size));
                ok = std::fread(data.data(), 1, data.size(), f) == data.size();
            }
            std::fclose(f);
            return ok;
        }
        inline uint32_t rd32(const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

        // The DDS (DXT5 or 32-bit BGRA, level 0) as a BC3 chain down to 1 x 1, with the alpha figures.
        bool load(const Job &job, Done &out)
        {
            const Entry &e = g_entries[job.handle - 1u];
            std::vector<uint8_t> file;
            if (!readFile(e.name, file) || std::memcmp(file.data(), "DDS ", 4) != 0)
                return false;
            const uint32_t h = rd32(&file[12]), w = rd32(&file[16]);
            if (!w || !h || w > 4096 || h > 4096 || (w & (w - 1)) || (h & (h - 1)))
                return false;
            const uint32_t pfFlags = rd32(&file[80]), four = rd32(&file[84]), bitCount = rd32(&file[88]);
            const uint32_t rMask = rd32(&file[92]), aMask = rd32(&file[104]);
            const size_t l0 = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 16u;
            std::vector<uint8_t> rgba, next;
            out.bc3.clear();
            if ((pfFlags & 4u) && four == 0x35545844u) // 'DXT5'
            {
                if (file.size() < 128u + l0)
                    return false;
                out.bc3.assign(file.begin() + 128, file.begin() + 128 + static_cast<long>(l0));
                decodeImage(out.bc3.data(), w, h, rgba);
            }
            else if (!(pfFlags & 4u) && bitCount == 32u && aMask == 0xFF000000u && (rMask == 0x00FF0000u || rMask == 0x000000FFu))
            {
                const size_t bytes = static_cast<size_t>(w) * h * 4u;
                if (file.size() < 128u + bytes)
                    return false;
                rgba.assign(file.begin() + 128, file.begin() + 128 + static_cast<long>(bytes));
                if (rMask == 0x00FF0000u)
                    for (size_t i = 0; i < bytes; i += 4)
                        std::swap(rgba[i], rgba[i + 2]);
                encodeImage(rgba, w, h, out.bc3);
            }
            else
                return false;
            // The alpha figures (as uploadTexture: permille of texels >= 128 / <= 2 at level 0; the least and
            // most alpha over every level, here level 0's: box filtering keeps the range inside it).
            uint8_t lo = 255, hi = 0;
            size_t full = 0, clear = 0;
            for (size_t i = 3; i < rgba.size(); i += 4)
            {
                const uint8_t a = rgba[i];
                lo = std::min(lo, a), hi = std::max(hi, a);
                full += a >= 128, clear += a <= 2;
            }
            const size_t texels = rgba.size() / 4u;
            out.t.minAlpha = lo, out.t.maxAlpha = hi;
            out.t.fullTexels = static_cast<uint16_t>(full * 1000u / texels), out.t.clearTexels = static_cast<uint16_t>(clear * 1000u / texels);
            // The alpha grid at the PS2 texture's size (the most alpha of each area: where a sample can pass).
            if (job.gridWanted && out.t.clearTexels >= 200 && job.ps2W >= 4 && job.ps2H >= 4 && job.ps2W <= 256 && job.ps2H <= 256 && w >= job.ps2W && h >= job.ps2H)
            {
                const uint32_t sx = w / job.ps2W, sy = h / job.ps2H;
                out.grid.assign(static_cast<size_t>(job.ps2W) * job.ps2H, 0);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        uint8_t &g = out.grid[(y / sy) * job.ps2W + x / sx];
                        g = std::max(g, rgba[(static_cast<size_t>(y) * w + x) * 4u + 3]);
                    }
                out.t.alphaGrid = out.grid.data(), out.t.gridW = job.ps2W, out.t.gridH = job.ps2H;
            }
            uint32_t levels = 1;
            std::vector<uint8_t> dilated;
            // v168: cut-out textures (foliage, fences): each smaller level's alpha scaled so the share of texels
            // passing an alpha test (0x40, half of the PS2's opaque 0x80) stays level 0's. Plain box filtering
            // thinned them level by level until far foliage failed the test and vanished (v167d's cinematic).
            const bool cutout = out.t.clearTexels >= 30 && out.t.fullTexels >= 30;
            constexpr uint8_t kRef = 0x40;
            size_t pass0 = 0;
            if (cutout)
                for (size_t i = 3; i < rgba.size(); i += 4)
                    pass0 += rgba[i] >= kRef;
            const double coverage0 = cutout ? double(pass0) / double(texels) : 0.0;
            auto keepCoverage = [&](std::vector<uint8_t> &img, uint32_t lw, uint32_t lh)
            {
                const size_t n = static_cast<size_t>(lw) * lh;
                if (!cutout || n < 4u)
                    return;
                auto passing = [&](double scale)
                {
                    size_t c = 0;
                    for (size_t i = 0; i < n; ++i)
                        c += std::min(255.0, img[i * 4u + 3] * scale) >= kRef;
                    return double(c) / double(n);
                };
                if (passing(1.0) >= coverage0)
                    return;
                double lo = 1.0, hi = 8.0;
                for (int step = 0; step < 12; ++step)
                {
                    const double mid = 0.5 * (lo + hi);
                    (passing(mid) < coverage0 ? lo : hi) = mid;
                }
                const double scale = hi;
                for (size_t i = 0; i < n; ++i)
                {
                    uint8_t &a = img[i * 4u + 3];
                    a = static_cast<uint8_t>(std::min<double>(out.t.maxAlpha, a * scale + 0.5));
                }
            };
            for (uint32_t lw = w, lh = h; lw > 1 || lh > 1; ++levels)
            {
                halve(rgba, lw, lh, next);
                lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2);
                keepCoverage(next, lw, lh);
                encodeImage(next, lw, lh, out.bc3);
                rgba.swap(next);
                // The grid also bounds a sample of this level: a level at least the grid's size by its
                // areas' maxima (a bilinear tap stays within the lookup's one-cell margin), a smaller one
                // by its texels' 3 x 3 maxima (the texture repeating).
                if (out.grid.empty())
                    continue;
                const uint32_t gw = job.ps2W, gh = job.ps2H;
                if (lw >= gw && lh >= gh)
                {
                    const uint32_t sx = lw / gw, sy = lh / gh;
                    for (uint32_t y = 0; y < lh; ++y)
                        for (uint32_t x = 0; x < lw; ++x)
                        {
                            uint8_t &g = out.grid[(y / sy) * gw + x / sx];
                            g = std::max(g, rgba[(static_cast<size_t>(y) * lw + x) * 4u + 3]);
                        }
                    continue;
                }
                dilated.assign(static_cast<size_t>(lw) * lh, 0);
                for (uint32_t y = 0; y < lh; ++y)
                    for (uint32_t x = 0; x < lw; ++x)
                    {
                        uint8_t m = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                m = std::max(m, rgba[(static_cast<size_t>((y + lh + dy) % lh) * lw + (x + lw + dx) % lw) * 4u + 3]);
                        dilated[static_cast<size_t>(y) * lw + x] = m;
                    }
                for (uint32_t y = 0; y < gh; ++y)
                    for (uint32_t x = 0; x < gw; ++x)
                    {
                        uint8_t &g = out.grid[static_cast<size_t>(y) * gw + x];
                        g = std::max(g, dilated[static_cast<size_t>(std::min(y * lh / gh, lh - 1)) * lw + std::min(x * lw / gw, lw - 1)]);
                    }
            }
            out.t.width = w, out.t.height = h, out.t.levels = levels;
            out.t.data = out.bc3.data(), out.t.bytes = out.bc3.size();
            return true;
        }

        void loop()
        {
            ThreadNaming::SetCurrentThreadName("TexPack");
            for (;;)
            {
                Job job;
                bool doScan = false;
                {
                    std::unique_lock<std::mutex> guard(g_lock);
                    g_wake.wait(guard, [] { return !g_jobs.empty() || (g_scanAsked && !g_scanned.load()); });
                    if (g_scanAsked && !g_scanned.load())
                        doScan = true;
                    else
                    {
                        job = g_jobs.front();
                        g_jobs.pop_front();
                    }
                }
                if (doScan)
                {
                    scan();
                    continue;
                }
                if (job.generation != g_generation.load(std::memory_order_acquire))
                    continue;
                if (g_bytesLoaded.load(std::memory_order_relaxed) >= budgetBytes())
                {
                    g_overBudget.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                auto done = std::make_unique<Done>();
                if (!load(job, *done))
                {
                    if (g_failed.fetch_add(1, std::memory_order_relaxed) < 8u)
                        std::fprintf(stderr, "[texpack] could not load %s\n", g_entries[job.handle - 1u].name.c_str());
                    continue;
                }
                done->key = job.key;
                done->generation = job.generation;
                done->t.data = done->bc3.data();
                if (!done->grid.empty())
                    done->t.alphaGrid = done->grid.data();
                g_bytesLoaded.fetch_add(done->bc3.size(), std::memory_order_relaxed);
                std::lock_guard<std::mutex> guard(g_lock);
                g_done.push_back(std::move(done));
            }
        }
        void startThread()
        {
            if (g_threadStarted)
                return;
            g_threadStarted = true;
            // v168: libnx's own thread (v167's std::thread threw on the game thread: std::terminate).
            auto body = [](void *)
            {
                try
                {
                    loop();
                }
                catch (...)
                {
                    std::fprintf(stderr, "[texpack] its thread stopped (exception)\n");
                }
            };
            if (!native_renderer::startWorkerThread(body, nullptr, 256u << 10))
                std::fprintf(stderr, "[texpack] could not start its thread\n");
        }

        void hashLevel(XXH3_state_t &st, uint32_t psm, uint32_t w, uint32_t h, const uint8_t *idx)
        {
            const bool blocks = psm == 0x13u ? (w >= 16u && h >= 16u) : (w >= 32u && h >= 16u);
            if (!blocks)
            {
                XXH3_64bits_update(&st, idx, static_cast<size_t>(w) * h);
                return;
            }
            uint8_t block[256];
            if (psm == 0x13u)
                for (uint32_t by = 0; by < h / 16u; ++by)
                    for (uint32_t bx = 0; bx < w / 16u; ++bx)
                    {
                        for (uint32_t y = 0; y < 16u; ++y)
                        {
                            const uint8_t *row = idx + static_cast<size_t>(by * 16u + y) * w + bx * 16u;
                            for (uint32_t x = 0; x < 16u; ++x)
                                block[kColumn8[y][x]] = row[x];
                        }
                        XXH3_64bits_update(&st, block, 256);
                    }
            else
                for (uint32_t by = 0; by < h / 16u; ++by)
                    for (uint32_t bx = 0; bx < w / 32u; ++bx)
                    {
                        std::memset(block, 0, sizeof(block));
                        for (uint32_t y = 0; y < 16u; ++y)
                        {
                            const uint8_t *row = idx + static_cast<size_t>(by * 16u + y) * w + bx * 32u;
                            for (uint32_t x = 0; x < 32u; ++x)
                            {
                                const uint32_t n = kColumn4[y][x];
                                block[n >> 1] |= static_cast<uint8_t>((row[x] & 15u) << ((n & 1u) * 4u));
                            }
                        }
                        XXH3_64bits_update(&st, block, 256);
                    }
        }
        uint64_t texHash(const Source &s, bool mips)
        {
            XXH3_state_t st;
            XXH3_64bits_reset(&st);
            const uint8_t *idx = s.indices;
            for (uint32_t level = 0, w = s.width, h = s.height; level < (mips ? s.levels : 1u) && w >= 1u && h >= 1u; ++level, w /= 2u, h /= 2u)
            {
                hashLevel(st, s.psm, w, h, idx);
                idx += static_cast<size_t>(w) * h;
            }
            return XXH3_64bits_digest(&st);
        }
        uint32_t log2u(uint32_t v)
        {
            uint32_t r = 0;
            while ((1u << r) < v)
                ++r;
            return r;
        }
        uint32_t match(uint64_t tex, uint64_t clut, uint32_t bits)
        {
            const auto range = g_byTex.equal_range(tex);
            for (auto it = range.first; it != range.second; ++it)
            {
                const Entry &e = g_entries[it->second];
                if (e.clut == clut && (e.bits & 0x3FFFu) == bits)
                    return it->second + 1u;
            }
            return 0;
        }
    }

    uint64_t keySalt() { return g_salt; }

    uint32_t find(const Source &s)
    {
        if (!g_scanned.load(std::memory_order_acquire) || (s.psm != 0x13u && s.psm != 0x14u) || !s.indices || !s.palette || g_entries.empty())
            return 0;
        g_finds.fetch_add(1, std::memory_order_relaxed);
        const uint32_t bits = s.psm | log2u(s.width) << 6 | log2u(s.height) << 10;
        const uint64_t clut = XXH3_64bits(s.palette, (s.psm == 0x14u ? 16u : 256u) * 4u);
        const uint64_t tex0 = texHash(s, false);
        if (const uint32_t h = match(tex0, clut, bits))
        {
            g_hits.fetch_add(1, std::memory_order_relaxed);
            return h;
        }
        uint64_t texMips = 0;
        if (s.levels > 1u)
        {
            texMips = texHash(s, true);
            if (const uint32_t h = match(texMips, clut, bits))
            {
                g_mipHits.fetch_add(1, std::memory_order_relaxed);
                return h;
            }
        }
        // The first misses, to compare with the pack's names offline.
        if (g_missLogged.fetch_add(1, std::memory_order_relaxed) < 40u)
            std::fprintf(stderr, "[texpack] miss psm %02x %ux%u levels %u tex %llx mips %llx clut %llx bits %05x\n", s.psm, s.width, s.height, s.levels,
                         (unsigned long long)tex0, (unsigned long long)texMips, (unsigned long long)clut, bits | 0x4000u);
        return 0;
    }

    void request(uint64_t key, uint32_t handle, uint32_t ps2Width, uint32_t ps2Height, bool gridWanted)
    {
        if (!handle || !g_salt)
            return;
        ++g_requests;
        std::lock_guard<std::mutex> guard(g_lock);
        g_jobs.push_back({key, handle, ps2Width, ps2Height, g_generation.load(std::memory_order_relaxed), gridWanted});
        g_wake.notify_one();
    }

    void frame()
    {
        auto &live = celosia::live::settings();
        const bool on = live.texturePack.load(std::memory_order_relaxed);
        ++g_frames;
        if (on && !g_wasOn)
        {
            startThread();
            std::lock_guard<std::mutex> guard(g_lock);
            g_scanAsked = true;
            g_wake.notify_one();
        }
        if (on && !g_salt && g_scanned.load(std::memory_order_acquire) && !g_entries.empty())
            g_salt = 0x01A5C3E7D2B49F61ull; // within the keys' low 58 bits (their tags stay)
        if (!on && g_wasOn)
        {
            // Off: the PS2 keys again (still held); the pack's textures freed, the loads in flight dropped.
            g_salt = 0;
            g_generation.fetch_add(1, std::memory_order_acq_rel);
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_jobs.clear();
                g_done.clear();
            }
            for (const uint64_t key : g_uploadedKeys)
                native_renderer::dropTexture(key);
            g_uploadedKeys.clear();
            g_bytesLoaded = 0;
            live.texturePackLoaded = 0;
            live.texturePackMB = 0;
        }
        g_wasOn = on;
        if (!g_salt)
            return;
        // At most ~2 MB and 8 textures of uploads a frame (v168: 6 MB, no count, gave 455 ms frames in the
        // temple's first seconds: each texture is a new GPU image and two copies).
        constexpr size_t kBudget = 2u << 20;
        size_t budget = kBudget;
        int uploadsThisFrame = 0;
        const auto uploadStart = std::chrono::steady_clock::now();
        for (;;)
        {
            std::unique_ptr<Done> done;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                if (g_done.empty() || uploadsThisFrame >= 8 || (budget < g_done.front()->bc3.size() && budget != kBudget))
                    break;
                done = std::move(g_done.front());
                g_done.pop_front();
            }
            budget -= std::min(budget, done->bc3.size());
            if (done->generation != g_generation.load(std::memory_order_relaxed))
            {
                ++g_staleDrops;
                continue;
            }
            ++g_uploadTries;
            ++uploadsThisFrame;
            if (native_renderer::uploadCompressedTexture(done->key, done->t))
            {
                g_uploadedKeys.push_back(done->key);
                live.texturePackLoaded = static_cast<uint32_t>(g_uploadedKeys.size());
                live.texturePackMB = static_cast<uint32_t>(g_bytesLoaded.load(std::memory_order_relaxed) >> 20);
            }
        }
        if (uploadsThisFrame)
        {
            static int slowLogs = 0;
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - uploadStart).count();
            if (us > 5000 && slowLogs++ < 20)
                std::fprintf(stderr, "[texpack] frame %llu: %d uploads took %lld us on the game thread\n", (unsigned long long)g_frames,
                             uploadsThisFrame, (long long)us);
        }
        if (g_frames % 600u == 0u)
            std::fprintf(stderr, "[texpack] looked %llu hits %llu (mips %llu) requested %llu tries %llu stale %llu replaced %zu loaded %llu MB over-budget %llu failed %llu\n",
                         (unsigned long long)g_finds.load(), (unsigned long long)g_hits.load(), (unsigned long long)g_mipHits.load(),
                         (unsigned long long)g_requests, (unsigned long long)g_uploadTries, (unsigned long long)g_staleDrops, g_uploadedKeys.size(),
                         (unsigned long long)(g_bytesLoaded.load() >> 20), (unsigned long long)g_overBudget.load(), (unsigned long long)g_failed.load());
    }
}
