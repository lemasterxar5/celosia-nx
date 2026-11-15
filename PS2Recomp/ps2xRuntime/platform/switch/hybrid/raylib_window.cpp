#include "raylib.h"
#include "runtime/native_renderer.h"
#include <switch.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

void pollSwitchInput();
namespace {
bool ready=false,drawing=false;
int logicalWidth=1280,logicalHeight=720,targetFps=60;
double lastFrame=0,frameDuration=0;
uint32_t nextTexture=1,drawIndex=0;
constexpr uint64_t HostId=0xFFFF000000000000ull;
struct Pixels {int width,height;std::vector<uint8_t> rgba;bool dirty=true;};
std::unordered_map<uint32_t,Pixels> textures;
size_t imageBytes(int width,int height)
{
    if(width<=0 || height<=0 || width>4096 || height>4096)
        throw std::invalid_argument("Switch host image dimensions exceed bounds");
    return size_t(width)*height*4;
}
uint8_t colourByte(uint8_t value){return uint8_t((unsigned(value)*128+127)/255);}
void quad(uint64_t texture,Rectangle uv,Rectangle destination,Vector2 origin,float rotation,Color tint)
{
    if(!drawing)return;
    if(drawIndex==65535)throw std::runtime_error("Switch host drawing capacity exceeded");
    const float angle=rotation*0.01745329251994329577f,c=std::cos(angle),s=std::sin(angle);
    std::array<native_renderer::Vertex,6> vertices{};
    constexpr std::array<unsigned,6> indices={0,1,2,0,2,3};
    constexpr std::array<float,4> xs={0,1,1,0},ys={0,0,1,1};
    for(unsigned i=0;i<vertices.size();++i) {
        const auto corner=indices[i];const auto x=xs[corner]*destination.width-origin.x,y=ys[corner]*destination.height-origin.y;
        auto &v=vertices[i];v.x=(destination.x+x*c-y*s)*2/logicalWidth-1;
        v.y=1-(destination.y+x*s+y*c)*2/logicalHeight;v.z=0;
        v.u=uv.x+xs[corner]*uv.width;v.v=uv.y+ys[corner]*uv.height;
        // Textured RGB is modulated at 128=1; untextured RGB retains the
        // framebuffer's 0..255 scale (the shader supplies its half-scale texel).
        v.r=texture?colourByte(tint.r):tint.r;v.g=texture?colourByte(tint.g):tint.g;
        v.b=texture?colourByte(tint.b):tint.b;v.a=colourByte(tint.a);
    }
    const auto mesh=HostId|0x100000000ull|++drawIndex;
    native_renderer::uploadMesh(mesh,vertices.data(),6,true);
    constexpr float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    native_renderer::DrawState state;state.texture=texture;state.screen=true;state.noDepthWrite=true;
    state.clamp=3;state.order=drawIndex;native_renderer::draw(mesh,identity,state);
}
#include "../../vita/raylib/default_font.inc"
struct Glyph {int x,y,w;};
std::array<Glyph,224> glyphs{};
Texture2D font{};
void ensureFont()
{
    if(font.id)return;
    std::vector<uint32_t> pixels(128*128);
    for(unsigned i=0;i<pixels.size();++i)pixels[i]=(kDefaultFontBits[i/32]&(1u<<(i%32)))?0xFFFFFFFFu:0x00FFFFFFu;
    Image image{pixels.data(),128,128,1,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};font=LoadTextureFromImage(image);
    int line=0,x=1,test=1;
    for(unsigned i=0;i<glyphs.size();++i){const int w=kDefaultFontWidths[i];glyphs[i]={x,1+line*11,w};test+=w+1;
        if(test>=128){++line;x=2+w;test=x;glyphs[i]={1,1+line*11,w};}else x=test;}
}
int codepoint(const char *&p)
{
    const auto first=static_cast<uint8_t>(*p++);if(first<128)return first;
    if((first&0xE0)==0xC0 && (static_cast<uint8_t>(*p)&0xC0)==0x80)
        return ((first&31)<<6)|(static_cast<uint8_t>(*p++)&63);
    while((static_cast<uint8_t>(*p)&0xC0)==0x80)++p;return '?';
}
}
void InitWindow(int width,int height,const char *)
{if(width<=0 || height<=0)throw std::invalid_argument("Invalid host logical dimensions");logicalWidth=width;logicalHeight=height;ready=true;}
void CloseWindow(){if(drawing)EndDrawing();native_renderer::finishFrames();textures.clear();font={};ready=false;}
bool IsWindowReady(){return ready;}
bool WindowShouldClose(){return !ready || !appletMainLoop();}
void SetConfigFlags(unsigned int){}
void SetTargetFPS(int fps){if(fps<0 || fps>240)throw std::invalid_argument("Invalid frame cap");targetFps=fps;}
int GetFPS(){return frameDuration>0?int(1/frameDuration+0.5):0;}
double GetTime(){
    // Match raylib's elapsed application clock. Console uptime makes timers
    // and periodic diagnostics attempt to catch up hours before this process.
    static const uint64_t base=armGetSystemTick();
    return double(armTicksToNs(armGetSystemTick()-base))/1000000000.0;
}
int GetScreenWidth(){return logicalWidth;}
int GetScreenHeight(){return logicalHeight;}
void PollInputEvents(){pollSwitchInput();}
void BeginDrawing()
{
    if(!ready || drawing)throw std::logic_error("Invalid Switch host frame lifecycle");
    PollInputEvents();drawIndex=0;
    // Native game frames already present directly through deko3d. Do not copy
    // their image back through the legacy host framebuffer path.
    drawing=!native_renderer::shown();if(drawing)native_renderer::beginFrame();
}
void EndDrawing()
{
    if(drawing){native_renderer::endFrame(1280,720);drawing=false;}
    const auto now=GetTime();
    if(targetFps && lastFrame && now-lastFrame<1.0/targetFps)
        svcSleepThread(int64_t((1.0/targetFps-(now-lastFrame))*1000000000.0));
    const auto end=GetTime();frameDuration=lastFrame?end-lastFrame:0;lastFrame=end;
}
void ClearBackground(Color color){DrawRectangle(0,0,logicalWidth,logicalHeight,color);}
void DrawRectangle(int x,int y,int width,int height,Color color)
{quad(0,{0,0,1,1},{float(x),float(y),float(width),float(height)},{0,0},0,color);}
void DrawTexturePro(Texture2D texture,Rectangle source,Rectangle dest,Vector2 origin,float rotation,Color tint)
{
    if(!drawing)return;const auto found=textures.find(texture.id);
    if(found==textures.end())throw std::invalid_argument("Unknown Switch host texture");
    auto &pixels=found->second;
    if(pixels.dirty){auto rgba=pixels.rgba;for(size_t i=3;i<rgba.size();i+=4)rgba[i]=colourByte(rgba[i]);
        native_renderer::uploadTexture(HostId|texture.id,pixels.width,pixels.height,rgba.data(),1);pixels.dirty=false;}
    if(source.height<0)source.y-=source.height;
    quad(HostId|texture.id,{source.x/pixels.width,source.y/pixels.height,source.width/pixels.width,source.height/pixels.height},dest,origin,rotation,tint);
}
Image GenImageColor(int width,int height,Color colour)
{
    const auto bytes=imageBytes(width,height);auto *pixels=static_cast<Color *>(std::malloc(bytes));
    if(!pixels)throw std::bad_alloc();std::fill_n(pixels,size_t(width)*height,colour);
    return {pixels,width,height,1,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
}
void UnloadImage(Image image){std::free(image.data);}
void ImageCrop(Image *image,Rectangle area)
{
    if(!image || !image->data || image->format!=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)throw std::invalid_argument("Invalid RGBA image");
    (void)imageBytes(image->width,image->height);
    if(!std::isfinite(area.x)||!std::isfinite(area.y)||!std::isfinite(area.width)||!std::isfinite(area.height))throw std::invalid_argument("Invalid crop");
    if(area.x<0||area.y<0||area.width<1||area.height<1||double(area.x)+area.width>image->width||double(area.y)+area.height>image->height)
        throw std::invalid_argument("Crop outside image");
    const int x=int(area.x),y=int(area.y),width=int(area.width),height=int(area.height);
    auto output=GenImageColor(width,height,BLANK);
    for(int row=0;row<height;++row)std::memcpy(static_cast<uint8_t *>(output.data)+size_t(row)*width*4,
        static_cast<const uint8_t *>(image->data)+(size_t(row+y)*image->width+x)*4,size_t(width)*4);
    UnloadImage(*image);*image=output;
}
bool ExportImage(Image,const char *){return false;}
Texture2D LoadTextureFromImage(Image image)
{
    if(!image.data || image.format!=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 || textures.size()>=1024 || !nextTexture)
        throw std::invalid_argument("Unsupported Switch host texture");
    const auto count=imageBytes(image.width,image.height);const auto *pixels=static_cast<const uint8_t *>(image.data);
    const auto id=nextTexture++;textures.emplace(id,Pixels{image.width,image.height,{pixels,pixels+count},true});
    return {id,image.width,image.height,1,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
}
Image LoadImageFromTexture(Texture2D texture)
{
    const auto &pixels=textures.at(texture.id);auto image=GenImageColor(pixels.width,pixels.height,BLANK);
    std::memcpy(image.data,pixels.rgba.data(),pixels.rgba.size());return image;
}
void UpdateTexture(Texture2D texture,const void *data)
{if(!data)throw std::invalid_argument("Missing texture bytes");auto &pixels=textures.at(texture.id);std::memcpy(pixels.rgba.data(),data,pixels.rgba.size());pixels.dirty=true;}
void UnloadTexture(Texture2D texture){textures.erase(texture.id);}
void SetTextureFilter(Texture2D texture,int filter)
{
    if(!textures.count(texture.id) || filter!=TEXTURE_FILTER_BILINEAR)
        throw std::invalid_argument("Switch host textures support bilinear filtering");
    // The deko host texture sampler is already bilinear.
}
void DrawText(const char *text,int x,int y,int size,Color colour)
{
    if(!text || !drawing)return;ensureFont();size=std::clamp(size,10,1024);const float scale=size/10.0f;float at=x;
    for(const char *p=text;*p;){const int cp=codepoint(p);if(cp=='\n'){at=x;y+=size+size/2;continue;}
        const auto &g=glyphs[cp>=32&&cp<256?cp-32:'?'-32];
        if(cp!=' ')DrawTexturePro(font,{float(g.x),float(g.y),float(g.w),10},{at,float(y),g.w*scale,float(size)},{0,0},0,colour);
        at+=g.w*scale+size/10;}
}
int MeasureText(const char *text,int size)
{
    if(!text)return 0;size=std::clamp(size,10,1024);float width=0,best=0;
    for(const char *p=text;*p;){const int cp=codepoint(p);if(cp=='\n'){best=std::max(best,width);width=0;continue;}
        width+=kDefaultFontWidths[cp>=32&&cp<256?cp-32:'?'-32]*(size/10.0f)+size/10;}
    return int(std::max(best,width));
}
