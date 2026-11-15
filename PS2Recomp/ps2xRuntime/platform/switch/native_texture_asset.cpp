#include "native_texture_asset.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
namespace celosia::native {
namespace {
// Static NTO2 byte-layout permutations extracted from the existing asset
// converter. These are file offsets only: no GPU registers, memory or commands.
    static constexpr uint8_t blockTable32[4][8] = {
        {0, 1, 4, 5, 16, 17, 20, 21},
        {2, 3, 6, 7, 18, 19, 22, 23},
        {8, 9, 12, 13, 24, 25, 28, 29},
        {10, 11, 14, 15, 26, 27, 30, 31},
    };
    static constexpr uint8_t columnTable32[8][8] = {
        {0, 1, 4, 5, 8, 9, 12, 13},
        {2, 3, 6, 7, 10, 11, 14, 15},
        {16, 17, 20, 21, 24, 25, 28, 29},
        {18, 19, 22, 23, 26, 27, 30, 31},
        {32, 33, 36, 37, 40, 41, 44, 45},
        {34, 35, 38, 39, 42, 43, 46, 47},
        {48, 49, 52, 53, 56, 57, 60, 61},
        {50, 51, 54, 55, 58, 59, 62, 63},
    };
    static constexpr uint8_t blockTable8[4][8] = {
        {0, 1, 4, 5, 16, 17, 20, 21},
        {2, 3, 6, 7, 18, 19, 22, 23},
        {8, 9, 12, 13, 24, 25, 28, 29},
        {10, 11, 14, 15, 26, 27, 30, 31},
    };
    static constexpr uint8_t columnTable8[16][16] = {
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
    static constexpr uint8_t blockTable4[8][4] = {
        {0, 2, 8, 10},
        {1, 3, 9, 11},
        {4, 6, 12, 14},
        {5, 7, 13, 15},
        {16, 18, 24, 26},
        {17, 19, 25, 27},
        {20, 22, 28, 30},
        {21, 23, 29, 31},
    };
    static constexpr uint16_t columnTable4[16][32] = {
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
constexpr uint32_t address32(uint32_t x,uint32_t y)
{
    return uint32_t(blockTable32[(y>>3)&3][(x>>3)&7])*256+
           uint32_t(columnTable32[y&7][x&7])*4;
}
constexpr auto inverse32()
{
    std::array<uint16_t,2048> result{};
    for(uint32_t y=0;y<32;++y)for(uint32_t x=0;x<64;++x)
        result[address32(x,y)/4]=uint16_t(x|(y<<6));
    return result;
}
constexpr auto Inverse32=inverse32();
uint32_t indexedAddress(uint32_t psm,uint32_t width,uint32_t x,uint32_t y)
{
    const auto pages=std::max(width/128,1u);
    if(psm==0x13) {
        const auto page=(y/64)*pages+x/128;
        const auto block=blockTable8[(y/16)&3][(x/16)&7];
        return page*8192+uint32_t(block)*256+columnTable8[y&15][x&15];
    }
    const auto page=(y/128)*pages+x/128;
    const auto block=blockTable4[(y/16)&7][(x/32)&3];
    return page*16384+uint32_t(block)*512+columnTable4[y&15][x&31];
}
[[noreturn]] void malformed(){throw std::invalid_argument("Unsupported or truncated native NTO2 texture asset");}
void bounds(std::span<const uint8_t> bytes,size_t offset,size_t count)
{if(offset>bytes.size() || count>bytes.size()-offset)malformed();}
uint32_t word(std::span<const uint8_t> bytes,size_t offset)
{
    bounds(bytes,offset,4);uint32_t value=0;
    for(unsigned i=0;i<4;++i)value|=uint32_t(bytes[offset+i])<<(i*8);
    return value;
}
struct Description {
    uint32_t psm,width,height,levels,swizzles;size_t pixels;
    std::array<uint32_t,256> palette{};
};
Description describe(std::span<const uint8_t> bytes)
{
    bounds(bytes,0,32);if(word(bytes,0)!=0x324F544E)malformed();
    const auto format=word(bytes,0x1C);
    Description result{format&63,1u<<((format>>16)&15),1u<<((format>>20)&15),
        std::max((format>>12)&15,1u),bytes[0x1F],word(bytes,0x14),{}};
    if((result.psm!=0 && result.psm!=0x13 && result.psm!=0x14) ||
       result.width>1024 || result.height>1024 || result.levels>7 || result.pixels<32)malformed();
    if(result.psm!=0) {
        const size_t start=word(bytes,0x18);const auto entries=result.psm==0x14?16u:256u;
        if(start<32)malformed();
        bounds(bytes,start,size_t(entries)*4);
        for(uint32_t i=0;i<entries;++i) {
            const auto from=entries==256?((i&~0x18u)|((i&8)<<1)|((i&16)>>1)):i;
            result.palette[i]=word(bytes,start+from*4);
        }
    }
    return result;
}
size_t packedSize(uint32_t psm,uint32_t w,uint32_t h)
{return psm==0?size_t(w)*h*4:psm==0x13?size_t(w)*h:size_t(w)*h/2;}
size_t readSize(const Description &source,uint32_t level,uint32_t w,uint32_t h)
{
    if(source.psm && ((source.swizzles>>level)&1))
        return size_t(std::max(w/2,1u))*std::max(source.psm==0x13?h/2:h/4,1u)*4;
    return source.psm==0x14?(size_t(w)*h+1)/2:packedSize(source.psm,w,h);
}
// Convert a tiled asset offset directly back to its source packed-RGBA byte.
// Offsets outside the uploaded rectangle are defined zero in the NTO2 layout.
uint8_t tiledByte(std::span<const uint8_t> bytes,uint32_t psm,uint32_t w,uint32_t h,uint32_t at)
{
    const auto cw=std::max(w/2,1u),ch=std::max(psm==0x13?h/2:h/4,1u);
    const auto pages=std::max(cw/64,1u);
    const auto q=Inverse32[(at&8191)/4];const auto page=at/8192;
    const auto x=(page%pages)*64+(q&63),y=(page/pages)*32+(q>>6);
    if(x>=cw || y>=ch)return 0;
    return bytes[(size_t(y)*cw+x)*4+(at&3)];
}
uint32_t indexAt(std::span<const uint8_t> bytes,const Description &source,uint32_t level,uint32_t w,uint32_t h,uint32_t x,uint32_t y)
{
    if(!((source.swizzles>>level)&1)) {
        const auto index=size_t(y)*w+x;
        if(source.psm==0x13)return bytes[index];
        return (bytes[index/2]>>((x&1)*4))&15;
    }
    const auto at=indexedAddress(source.psm,w,x,y);
    if(source.psm==0x13)return tiledByte(bytes,source.psm,w,h,at);
    return (tiledByte(bytes,source.psm,w,h,at/2)>>((at&1)*4))&15;
}
}
NativeTextureAsset decodeNativeTextureAsset(std::span<const uint8_t> bytes)
{
    const auto source=describe(bytes);
    NativeTextureAsset result;
    size_t input=source.pixels,total=0;
    for(uint32_t level=0,w=source.width,h=source.height;level<source.levels && w && h;++level,w/=2,h/=2) {
        bounds(bytes,input,readSize(source,level,w,h));
        result.levels.push_back({w,h,total});total+=size_t(w)*h*4;
        input+=packedSize(source.psm,w,h);
    }
    result.rgba.resize(total);input=source.pixels;
    for(size_t level=0;level<result.levels.size();++level) {
        const auto &shape=result.levels[level];const auto w=shape.width,h=shape.height;
        auto *out=result.rgba.data()+shape.offset;
        const auto pixels=bytes.subspan(input,readSize(source,uint32_t(level),w,h));
        if(source.psm==0)std::copy_n(pixels.begin(),size_t(w)*h*4,out);
        else for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
            const auto color=source.palette[indexAt(pixels,source,uint32_t(level),w,h,x,y)];
            const auto at=(size_t(y)*w+x)*4;
            for(unsigned channel=0;channel<4;++channel)out[at+channel]=uint8_t(color>>(channel*8));
        }
        input+=packedSize(source.psm,w,h);
    }
    return result;
}
}
