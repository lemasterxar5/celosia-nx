#include "../native_faults.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace celosia::native;
namespace {
void require(bool ok,const char *message) { if(!ok) throw std::runtime_error(message); }
}
int main()
{
    const NativeFaultInfo fault{0x0123456789abcdef,0xfedcba9876543210,0,UINT64_MAX,0x96000004,0x101};
    const std::string expected="Native ARM64 fatal fault: description=0x00000101 esr=0x96000004 pc=0x0123456789abcdef lr=0xfedcba9876543210 sp=0x0000000000000000 far=0xffffffffffffffff\n";
    std::array<char,256> full{};
    require(formatNativeFault(fault,full)==expected.size(),"full written length");
    require(std::string(full.data())==expected,"register dump formatting");
    require(formatNativeFault(fault,{})==0,"empty/null output");
    // Test every truncation point with canaries around the exact writable span.
    for(std::size_t capacity=1;capacity<=expected.size()+2;++capacity) {
        std::array<char,256> guarded;
        guarded.fill('@');
        const auto written=formatNativeFault(fault,{guarded.data()+1,capacity});
        const auto wanted=capacity-1<expected.size()?capacity-1:expected.size();
        require(written==wanted,"truncation length");
        require(guarded[0]=='@' && guarded[capacity+1]=='@',"output canaries preserved");
        require(guarded[written+1]=='\0',"terminator inside output");
        require(std::string(guarded.data()+1)==expected.substr(0,wanted),"exact truncated prefix");
    }
    std::array<char,256> zero{};
    formatNativeFault({},zero);
    require(std::string(zero.data()).find("description=0x00000000 esr=0x00000000")!=std::string::npos,"zero fields");
#ifndef __SWITCH__
    NativeFaults unavailable;
    require(!unavailable.initialized() && !unavailable.initialize() && !unavailable.initialized(),"host cannot claim native hook");
#endif
    std::puts("native fault formatter checks passed");
}
