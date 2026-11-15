#include "../frame_rate.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
int main(){
    native_renderer::detail::deko::FrameRateWindow rate;
    unsigned checks=0;auto check=[&](bool v){++checks;if(!v)throw std::runtime_error("Frame-rate measurement regression");};
    check(!rate.record(100,true));
    for(unsigned i=1;i<300;++i)check(!rate.record(100+uint64_t(i)*5000000000ull/300,true));
    auto sample=rate.record(5000000100ull,true);check(sample&&sample->frames==300&&std::abs(sample->fps-60)<0.0001);
    for(unsigned i=1;i<=300;++i){auto s=rate.record(5000000100ull+uint64_t(i)*5000000000ull/300,i%2==0);if(s)check(s->frames==150&&std::abs(s->fps-30)<0.0001);}
    sample=rate.record(15000000100ull,false);check(sample&&sample->frames==0&&sample->fps==0);
    check(!rate.record(1,true));check(!rate.record(2,true));
    std::printf("frame rate: %u checks passed\n",checks);
}
