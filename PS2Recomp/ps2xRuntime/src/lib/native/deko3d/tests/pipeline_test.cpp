#include "../pipeline.hpp"
#include <cstdio>
#include <stdexcept>
using namespace native_renderer;
using detail::deko::pipelineKey;
int main(){
    unsigned checks=0;auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("Pipeline regression");};
    DrawState d;auto key=pipelineKey(d,false);check(key.depth==3&&key.mask==15);
    d.screen=true;check(pipelineKey(d,false).depth==0);
    d.screenDepthTest=true;check(pipelineKey(d,false).depth==1);
    d={};d.blend=Blend::Additive;check(pipelineKey(d,false).depth==1);
    d.writeDepth=true;check(pipelineKey(d,false).depth==3);
    d.noDepthWrite=true;check(pipelineKey(d,false).depth==1);
    d={};d.overlay=true;check(pipelineKey(d,false).depth==1);
    d={};d.alphaTest=2<<12;key=pipelineKey(d,true);check(key.mask==0&&key.depth==3);
    d.alphaTest=3<<12;key=pipelineKey(d,true);check(key.mask==7&&key.depth==1);
    d.alphaTest=1<<12;key=pipelineKey(d,true);check(key.mask==15&&key.depth==1);
    check(pipelineKey(d,false).mask==15&&pipelineKey(d,false).depth==3);
    // In the direct path, sky/far scenery is a backdrop, as in GL/Metal.
    // Its own depth must not reject overlapping cloud layers or the world.
    d={};d.background=true;d.blend=Blend::Normal;
    check(pipelineKey(d,false).depth==0);
    d.blend=Blend::Opaque;check(pipelineKey(d,false).depth==0);
    d.blend=Blend::Additive;check(pipelineKey(d,false).depth==0);
    d.writeDepth=true;check(pipelineKey(d,false).depth==0);
    d.screenDepthTest=true;check(pipelineKey(d,false).depth==1);
    d.blend=Blend::Normal;check(pipelineKey(d,false).depth==1);
    d.blend=Blend::Opaque;check(pipelineKey(d,false).depth==1);
    d.alphaTest=2<<12;check(pipelineKey(d,true).depth==1);
    d={};d.screenDepthTest=true;check(pipelineKey(d,false).depth==1);
    std::printf("pipeline: %u checks passed\n",checks);
}
