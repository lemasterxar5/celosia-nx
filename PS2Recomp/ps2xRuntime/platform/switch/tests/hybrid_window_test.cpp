#include "raylib.h"
#include "runtime/native_renderer.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace {bool show=false;unsigned begins=0,ends=0,draws=0;std::array<native_renderer::Vertex,6> last{};uint8_t uploadedAlpha=0;}
void pollSwitchInput(){}
namespace native_renderer {
bool shown(){return show;}
void beginFrame(){++begins;}
void endFrame(uint32_t width,uint32_t height){if(width!=1280||height!=720)throw std::runtime_error("Native resolution changed");++ends;}
void finishFrames(){}
void uploadMesh(uint64_t,const Vertex *vertices,uint32_t count,bool){if(count!=6)throw std::runtime_error("Invalid quad");std::copy_n(vertices,6,last.begin());}
void draw(uint64_t,const float *,const DrawState &state){if(!state.screen||!state.noDepthWrite)throw std::runtime_error("Invalid host material");++draws;}
void uploadTexture(uint64_t,uint32_t,uint32_t,const uint8_t *data,uint32_t){uploadedAlpha=data[3];}
}
int main()
{
    unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Native host window regression");};
    check(!IsWindowReady());InitWindow(640,360,"test");SetTargetFPS(0);check(IsWindowReady()&&!WindowShouldClose());
    check(GetTime()>=0 && GetTime()<1); // elapsed application time, never OS uptime
    BeginDrawing();DrawRectangle(0,0,640,360,WHITE);EndDrawing();
    check(begins==1&&ends==1&&draws==1&&last[0].x==-1&&last[0].y==1&&last[2].x==1&&last[2].y==-1);
    check(last[0].r==255&&last[0].a==128);
    auto image=GenImageColor(2,2,RED);auto texture=LoadTextureFromImage(image);
    ImageCrop(&image,{1,1,1,1});check(image.width==1&&image.height==1&&static_cast<uint8_t *>(image.data)[0]==230);UnloadImage(image);
    BeginDrawing();DrawTexturePro(texture,{0,0,2,-2},{0,0,640,360},{0,0},0,WHITE);EndDrawing();
    check(uploadedAlpha==128&&last[0].v==1&&last[2].v==0);
    const std::array<uint8_t,16> replacement={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};UpdateTexture(texture,replacement.data());
    image=LoadImageFromTexture(texture);check(!std::memcmp(image.data,replacement.data(),16));UnloadImage(image);
    const auto previous=draws;show=true;BeginDrawing();DrawRectangle(0,0,1,1,RED);EndDrawing();check(draws==previous&&ends==2);
    show=false;BeginDrawing();DrawText("A",0,0,10,WHITE);EndDrawing();check(draws>previous&&MeasureText("AA",10)>MeasureText("A",10));
    UnloadTexture(texture);CloseWindow();check(!IsWindowReady());
    std::printf("hybrid host window: %u checks passed\n",checks);
}
