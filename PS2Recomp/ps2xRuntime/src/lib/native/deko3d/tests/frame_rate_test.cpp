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
    // Per-frame minimums: 299 regular 16.67 ms gaps plus one 200 ms stall must surface as the
    // worst frame and dominate the 1% low (3 slowest of 300 -> (200+16.67+16.67)/3 ms).
    native_renderer::detail::deko::FrameRateWindow lows;
    check(!lows.record(0,true));
    uint64_t t=0;const uint64_t step=5000000000ull/300;
    for(unsigned i=1;i<300;++i){t+=step;check(!lows.record(t,true));}
    t+=200000000ull;auto low=lows.record(t,true);
    check(low&&low->frames==300&&low->intervalCount==300&&low->worstFrameNs==200000000ull);
    check(std::abs(low->worstFps()-5.0)<0.0001);
    const uint64_t expectedLow1=(200000000ull+2*step)/3;
    check(low->low1FrameNs==expectedLow1&&std::abs(low->low1Fps()-1e9/double(expectedLow1))<0.0001);
    // Held (non-presented) frames never shorten intervals: a window with a held frame in the
    // middle of two presentations reports the full gap, and a window with one presentation has none.
    native_renderer::detail::deko::FrameRateWindow heldGaps;
    check(!heldGaps.record(0,true));check(!heldGaps.record(1000000000ull,false));
    check(!heldGaps.record(2000000000ull,true));
    auto heldSample=heldGaps.record(5000000000ull,false);
    check(heldSample&&heldSample->frames==1&&heldSample->intervalCount==1&&heldSample->worstFrameNs==2000000000ull);
    // The gap across a window boundary belongs to the next window.
    auto next=heldGaps.record(5000000000ull+3000000000ull,true);check(!next);
    next=heldGaps.record(10000000001ull,true);
    check(next&&next->intervalCount==2&&next->worstFrameNs==6000000000ull);
    std::printf("frame rate: %u checks passed\n",checks);
}
