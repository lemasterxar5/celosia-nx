#include "native_texture_asset.hpp"
#include "../../../include/runtime/gs/ps2_gs_psmct32.h"
#include "../../../include/runtime/gs/ps2_gs_psmt8.h"
#include "../../../include/runtime/gs/ps2_gs_psmt4.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool value,const char *why){++checks;if(!value)throw std::runtime_error(why);}
void word(std::vector<uint8_t> &bytes,size_t at,uint32_t value)
{for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(8*i));}
std::vector<uint8_t> fixture(uint32_t format,size_t count)
{
    std::vector<uint8_t> bytes(0x420+count);word(bytes,0,0x324F544E);
    word(bytes,0x14,0x420);word(bytes,0x18,0x20);word(bytes,0x1C,format);
    for(unsigned i=0;i<256;++i)word(bytes,0x20+i*4,0x80000000u+i);
    return bytes;
}
bool fails(const std::vector<uint8_t> &bytes)
{try{(void)decodeNativeTextureAsset(bytes);}catch(const std::invalid_argument &){return true;}return false;}
uint32_t pixel(const NativeTextureAsset &texture,size_t index)
{uint32_t value=0;for(unsigned i=0;i<4;++i)value|=uint32_t(texture.rgba.at(index*4+i))<<(8*i);return value;}
void compareTiled(uint32_t psm,unsigned logWidth,unsigned logHeight)
{
    const auto w=1u<<logWidth,h=1u<<logHeight,cw=std::max(w/2,1u),ch=std::max(psm==0x13?h/2:h/4,1u);
    auto input=fixture(psm|0x1000|(logWidth<<16)|(logHeight<<20)|(1u<<24),size_t(cw)*ch*4);
    for(size_t i=0;i<size_t(cw)*ch*4;++i)input[0x420+i]=uint8_t(i*29+(i/127)*13);
    // Independent reference: materialize a zeroed intermediate asset-layout
    // buffer with the old scalar offset formulas, then sample it by index.
    std::vector<uint8_t> storage(size_t(std::max(w,64u))*std::max(h,64u)+0x4000);
    for(uint32_t y=0;y<ch;++y)for(uint32_t x=0;x<cw;++x) {
        const auto at=GSPSMCT32::addrPSMCT32(0,std::max(cw/64,1u),x,y);
        if(at+4<=storage.size())std::copy_n(input.data()+0x420+(size_t(y)*cw+x)*4,4,storage.data()+at);
    }
    const auto decoded=decodeNativeTextureAsset(input);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        const auto at=psm==0x13?GSPSMT8::addrPSMT8(0,std::max(w/64,2u),x,y):GSPSMT4::addrPSMT4(0,std::max(w/64,2u),x,y);
        const auto byteAt=psm==0x13?at:at/2;
        uint32_t index=byteAt<storage.size()?storage[byteAt]:0;
        if(psm==0x14)index=(index>>((at&1)*4))&15;
        const auto paletteIndex=psm==0x13?((index&~0x18u)|((index&8)<<1)|((index&16)>>1)):index;
        require(pixel(decoded,size_t(y)*w+x)==(0x80000000|paletteIndex),"all tiled pixels match independent packed-layout reference");
    }
}
int run()
{
    auto direct=fixture(0x1000|(1u<<16)|(1u<<20),16);
    for(unsigned i=0;i<16;++i)direct[0x420+i]=uint8_t(i*7);
    const auto rgba=decodeNativeTextureAsset(direct);
    require(rgba.levels.size()==1 && rgba.levels[0].width==2 && rgba.levels[0].height==2,"RGBA metadata");
    require(std::equal(rgba.rgba.begin(),rgba.rgba.end(),direct.begin()+0x420),"RGBA byte order and native alpha retained");
    auto indexed=fixture(0x13|0x1000|(2u<<16),4);
    indexed[0x420]=0;indexed[0x421]=8;indexed[0x422]=16;indexed[0x423]=255;
    const auto palette=decodeNativeTextureAsset(indexed);
    require(pixel(palette,0)==0x80000000 && pixel(palette,1)==0x80000010 && pixel(palette,2)==0x80000008 && pixel(palette,3)==0x800000FF,"CSM1 palette8/16 groups reordered");
    auto packed=fixture(0x14|0x1000|(2u<<16),2);packed[0x420]=0xA3;packed[0x421]=0xF0;
    const auto four=decodeNativeTextureAsset(packed);
    require(pixel(four,0)==0x80000003 && pixel(four,1)==0x8000000A && pixel(four,2)==0x80000000 && pixel(four,3)==0x8000000F,"indexed4 low nibble then high nibble");
    auto mips=fixture(0x3000|(2u<<16)|(2u<<20),84);
    for(unsigned i=0;i<84;++i)mips[0x420+i]=uint8_t(i);
    const auto chain=decodeNativeTextureAsset(mips);
    require(chain.levels.size()==3 && chain.levels[1].offset==64 && chain.levels[2].offset==80 && chain.rgba.size()==84,"mips contiguous and dimensions halved");
    require(chain.levels[2].width==1 && chain.rgba[83]==83,"last mip preserved");
    auto one=fixture(0x14|0x1000,1);one[0x420]=0xB7;
    require(pixel(decodeNativeTextureAsset(one),0)==0x80000007,"single pixel indexed4 requires one real input byte");
    auto swizzled8=fixture(0x13|0x1000|(7u<<16)|(6u<<20)|(1u<<24),8192);
    for(size_t i=0;i<8192;++i)swizzled8[0x420+i]=uint8_t(i);
    const auto tiled=decodeNativeTextureAsset(swizzled8);
    require(pixel(tiled,0)==0x80000000 && pixel(tiled,1)==0x80000004 && pixel(tiled,8)==0x80000002,"indexed8 tiled address golden bytes");
    auto swizzled4=fixture(0x14|0x1000|(7u<<16)|(7u<<20)|(1u<<24),8192);
    for(size_t i=0;i<8192;++i)swizzled4[0x420+i]=uint8_t(i);
    const auto tiled4=decodeNativeTextureAsset(swizzled4);
    require(pixel(tiled4,0)==0x80000000 && pixel(tiled4,1)==0x80000004 && pixel(tiled4,8)==0x80000001,"indexed4 tiled address golden nibbles");
    require(fails({}),"empty rejected");auto bad=direct;bad[0]=0;require(fails(bad),"magic rejected");
    bad=direct;word(bad,0x1C,0x12);require(fails(bad),"unknown format rejected");
    bad=direct;word(bad,0x1C,11u<<16);require(fails(bad),"oversized dimensions rejected");
    bad=direct;word(bad,0x1C,8u<<12);require(fails(bad),"unknown mip count rejected");
    bad=direct;word(bad,0x14,UINT32_MAX);require(fails(bad),"pixel pointer overflow rejected");
    bad=indexed;word(bad,0x18,UINT32_MAX);require(fails(bad),"palette pointer overflow rejected");
    bad=indexed;word(bad,0x18,4);require(fails(bad),"palette cannot overlap header");
    for(auto original:{direct,indexed,packed,mips,swizzled8,swizzled4,one}) {
        original.pop_back();require(fails(original),"truncated required payload never padded");
    }
    require(direct[0x420]==0 && indexed[0x421]==8,"inputs remain unchanged");
    for(const auto psm:{0x13u,0x14u})for(const auto dimensions:{std::array<unsigned,2>{0,0},{1,3},{4,4},{7,6},{8,8},{10,5}})
        compareTiled(psm,dimensions[0],dimensions[1]);
    std::printf("native texture asset: %u checks passed\n",checks);return 0;
}
}
int main(){try{return run();}catch(const std::exception &error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
