#include "../gpu_timing.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
int main(){
    using namespace native_renderer::detail::deko;
    unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("GPU timing regression");};
    GpuReport a{777,100},b{999,484};
    check(gpuIntervalNs(a,b)==625);
    check(gpuIntervalNs({0,100},{0,100})==0);
    check(!gpuIntervalNs({1,0},{1,400}));
    check(!gpuIntervalNs({1,400},{1,399}));
    check(gpuIntervalNs({1,1},{1,384000001})==625000000);
    check(!gpuIntervalNs({0,1},{0,std::numeric_limits<uint64_t>::max()}));
    check(sizeof(GpuReport)==16);
    GpuTimeWindow window;check(window.averageNs()==0 && window.count==0);
    check(window.add(100) && window.add(300) && window.averageNs()==200 && window.peakNs==300 && window.count==2);
    check(!window.add(std::numeric_limits<uint64_t>::max()) && window.count==2 && window.totalNs==400);
    window.reset();check(window.count==0 && window.totalNs==0 && window.peakNs==0);
    check(gpuCounterDelta({10,100},{45,200})==35);
    check(!gpuCounterDelta({10,0},{45,200}));
    check(!gpuCounterDelta({10,100},{45,100}));
    check(!gpuCounterDelta({45,100},{10,200}));
    check(!gpuCounterDelta({10,100},{10,200})); // zero cannot establish supported counter
    GpuCounterWindow counters;
    check(counters.add(GpuCounterValues{100,200,30}));
    check(counters.add(GpuCounterValues{300,600,90}));
    check(counters.count==2 && counters.averageVertices()==200 && counters.averageFragments()==400 && counters.averagePrimitives()==60);
    check(counters.fragmentInvocationsPerPixel(100)==4);
    check(!counters.add(GpuCounterValues{0,100,10}));
    check(!counters.add(std::nullopt));
    check(!counters.add(GpuCounterValues{std::numeric_limits<uint64_t>::max(),1,1}));
    check(counters.count==2 && counters.rejected==3 && counters.totalVertices==400);
    counters.reset();check(counters.count==0 && counters.rejected==0 && counters.totalFragments==0);
    check(counters.fragmentInvocationsPerPixel(0)==0);
    std::printf("GPU timing: %u checks passed\n",checks);
}
