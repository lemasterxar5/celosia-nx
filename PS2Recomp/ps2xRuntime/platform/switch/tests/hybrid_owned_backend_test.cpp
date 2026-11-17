#include "owned_backend.hpp"
#include "native_backend.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
namespace {
std::thread::id gpuThread;unsigned operations=0;uint8_t pixel=0;float position=0;bool fail=false;
unsigned indexedCommits=0,committedVertices=0;std::vector<uint32_t> committedIndices;
std::vector<float> committedPositions;
void owner(){if(std::this_thread::get_id()!=gpuThread)throw std::runtime_error("GPU operation escaped owner");++operations;}
struct Gpu final:native_renderer::detail::Backend {
    std::vector<native_renderer::Vertex> pendingVertices;
    std::vector<uint32_t> pendingIndices;
    native_renderer::Vertex *frameMesh(uint32_t count)override{owner();pendingVertices.resize(count);return pendingVertices.data();}
    uint32_t *frameMeshIndices(uint32_t count,uint32_t &base)override{owner();base=7;pendingIndices.resize(count);return pendingIndices.data();}
    void commitFrameMesh(uint64_t,uint32_t count,uint32_t indexCount)override{
        owner();++indexedCommits;committedVertices=count;
        committedIndices.assign(pendingIndices.begin(),pendingIndices.begin()+indexCount);
        committedPositions.push_back(pendingVertices[0].x);
    }
    ~Gpu() override{owner();}
    bool hasMesh(uint64_t) override{owner();return true;}
    bool hasTexture(uint64_t) override{owner();return true;}
    void uploadMesh(uint64_t,const native_renderer::Vertex *v,uint32_t,bool)override{owner();position=v[0].x;}
    void uploadTexture(uint64_t,uint32_t,uint32_t,const uint8_t *rgba,uint32_t)override{owner();pixel=rgba[0];}
    void beginFrame()override{owner();}
    void draw(uint64_t,const float *,const native_renderer::DrawState &)override{owner();if(fail)throw std::runtime_error("actual draw failure");}
    void setFog(const native_renderer::Fog &)override{owner();}
    void setBloom(const native_renderer::Bloom &)override{owner();}
    void addScreenBlur(const native_renderer::ScreenBlur &)override{owner();}
    void setGlowComposite(const native_renderer::GlowComposite &)override{owner();}
    void setDepthOfField(const native_renderer::DepthOfField &)override{owner();}
    void endFrame(uint32_t w,uint32_t h)override{owner();if(w!=1280||h!=720)throw std::runtime_error("Wrong raster size");}
    void finishFrames()override{owner();}
    bool latestFrame(std::vector<uint8_t>&,uint32_t &,uint32_t &)override{owner();return false;}
};
}
int main()
{
    unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Owned backend regression");};
    std::unique_ptr<native_renderer::detail::Backend> backend(native_renderer::detail::createOwnedDekoBackend([]{
        gpuThread=std::this_thread::get_id();return new Gpu;
    }));
    check(gpuThread!=std::this_thread::get_id());
    std::array<native_renderer::Vertex,3> vertices{};vertices[0].x=7;
    std::array<uint8_t,4> rgba={43,2,3,128};
    backend->uploadMesh(1,vertices.data(),3,false);backend->uploadTexture(2,1,1,rgba.data(),1);
    check(backend->hasMesh(1)&&backend->hasTexture(2));check(operations==0);
    vertices[0].x=99;rgba[0]=99;
    constexpr float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    backend->beginFrame();backend->setFog({});backend->setBloom({});backend->setGlowComposite({});
    backend->setDepthOfField({});backend->addScreenBlur({});backend->holdFrame();
    bool rejected=false;try{backend->endFrame(512,448);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    backend->draw(1,identity,{});backend->endFrame(1280,720);
    // endFrame hands the frame over asynchronously: finishFrames waits for it.
    backend->finishFrames();
    check(position==7&&pixel==43&&operations>=5);
    backend->beginFrame();
    native_renderer::DrawState sky;sky.background=true;
    backend->draw(1,identity,sky);
    native_renderer::DrawState world;world.fogged=true;
    backend->draw(1,identity,world);
    native_renderer::Fog depthFog;depthFog.enabled=true;depthFog.width=0; // an empty zFog ramp
    rejected=false;try{backend->setFog(depthFog);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    backend->endFrame(1280,720);
    backend->beginFrame();backend->uploadMesh(3,vertices.data(),3,true);
    backend->draw(3,identity,{});backend->endFrame(1280,720);
    check(!backend->hasMesh(3) && backend->hasMesh(1));
    backend->beginFrame();
    auto *dynamic=backend->frameMesh(4);dynamic[0].x=17;
    uint32_t base=99;auto *indices=backend->frameMeshIndices(6,base);check(base==0);
    const uint32_t quad[]={0,1,2,2,3,0};std::copy_n(quad,6,indices);
    backend->commitFrameMesh(4,4,6);backend->draw(4,identity,{});
    dynamic=backend->frameMesh(4);dynamic[0].x=23;
    indices=backend->frameMeshIndices(6,base);std::copy_n(quad,6,indices);
    backend->commitFrameMesh(4,4,6);dynamic[0].x=99;indices[0]=3;
    backend->endFrame(1280,720);backend->finishFrames();
    check(indexedCommits==2 && committedVertices==4);
    check(committedIndices==std::vector<uint32_t>({7,8,9,9,10,7}));
    check(committedPositions==std::vector<float>({17,23}));
    check(!backend->hasMesh(4));
    auto invalid=[&](auto operation){bool caught=false;try{operation();}catch(const std::invalid_argument &){caught=true;}check(caught);};
    invalid([&]{backend->frameMesh(0);});
    invalid([&]{backend->frameMeshIndices(3,base);});
    backend->beginFrame();dynamic=backend->frameMesh(4);dynamic[0].x=31;
    invalid([&]{backend->frameMeshIndices(4,base);});
    invalid([&]{backend->frameMeshIndices(16u*1024u*1024u+2u,base);});
    indices=backend->frameMeshIndices(6,base);std::copy_n(quad,6,indices);indices[5]=4;
    invalid([&]{backend->commitFrameMesh(5,4,6);});check(!backend->hasMesh(5));
    indices[5]=0;
    invalid([&]{backend->commitFrameMesh(5,5,6);});
    invalid([&]{backend->commitFrameMesh(5,4,9);});
    invalid([&]{backend->commitFrameMesh(0,4,6);});
    backend->cancelFrameMesh();invalid([&]{backend->commitFrameMesh(5,4,6);});
    dynamic=backend->frameMesh(3);dynamic[0].x=37;backend->commitFrameMesh(5,3,0);
    backend->draw(5,identity,{});backend->endFrame(1280,720);backend->finishFrames();
    check(indexedCommits==3 && committedVertices==3 && committedIndices.empty());
    check(committedPositions.back()==37 && !backend->hasMesh(5));
    backend->finishFrames();std::vector<uint8_t> image;uint32_t w=0,h=0;check(!backend->latestFrame(image,w,h));
    fail=true;backend->beginFrame();backend->draw(1,identity,{});
    // The asynchronous frame's failure reaches the next call that waits for it.
    bool failed=false;try{backend->endFrame(1280,720);backend->finishFrames();}catch(const std::runtime_error &){failed=true;}check(failed);
    rejected=false;try{backend->beginFrame();}catch(const std::runtime_error &){rejected=true;}check(rejected);
    backend.reset();check(operations>=9);
    std::unique_ptr<native_renderer::detail::Backend> capacity(native_renderer::detail::createOwnedDekoBackend([]{
        gpuThread=std::this_thread::get_id();return new Gpu;
    }));
    for(unsigned i=0;i<65536;++i)capacity->setFog({});
    auto *limited=capacity->frameMesh(3);limited[0].x=1;
    bool full=false;try{capacity->commitFrameMesh(9,3,0);}catch(const std::runtime_error &){full=true;}
    check(full && !capacity->hasMesh(9));capacity.reset();
    rejected=false;try{std::unique_ptr<native_renderer::detail::Backend> invalid(native_renderer::detail::createOwnedDekoBackend([]{return nullptr;}));}
    catch(const std::runtime_error &){rejected=true;}check(rejected);
    std::printf("hybrid GPU owner: %u checks passed\n",checks);
}
