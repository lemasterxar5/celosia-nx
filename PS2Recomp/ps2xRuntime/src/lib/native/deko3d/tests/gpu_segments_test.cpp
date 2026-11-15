#include "../gpu_segments.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace native_renderer::detail::deko;
int main(){
    unsigned checks=0;
    auto check=[&](bool ok){++checks;if(!ok){std::fprintf(stderr,"gpu segments check %u failed\n",checks);throw std::runtime_error("GPU segment regression");}};
    GpuSegmentRecorder r;
    check(r.change(GpuSegment::Opaque)==-1);            // inactive: no reports
    r.active=true;r.begin();
    check(r.change(GpuSegment::Prepass)==0);
    check(r.change(GpuSegment::Prepass)==-1);           // same kind: no new timestamp
    check(r.change(GpuSegment::Backdrop)==1);
    check(r.change(GpuSegment::Opaque)==2);
    check(r.change(GpuSegment::Backdrop)==3);
    check(r.close()==4 && r.count==5);
    check(r.close()==-1);
    // 384 ticks = 625 ns.
    std::vector<GpuReport> reports{{0,384},{0,768},{0,1536},{0,1920},{0,2304}};
    GpuSegmentWindow w;
    check(w.add(reports.data(),r.kinds.data(),r.count));
    check(w.frames==1);
    check(w.totalNs[unsigned(GpuSegment::Prepass)]==625);
    check(w.totalNs[unsigned(GpuSegment::Backdrop)]==1250+625);
    check(w.totalNs[unsigned(GpuSegment::Opaque)]==625);
    // Unwritten (zero) report: the whole frame is rejected.
    reports[2]={0,0};
    check(!w.add(reports.data(),r.kinds.data(),r.count) && w.frames==1);
    std::printf("deko gpu segments: %u checks passed\n",checks);
}
