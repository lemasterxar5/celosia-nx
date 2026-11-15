#include "native_printf.hpp"
#include "native_memory.hpp"
#include "native_aot_context.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    using namespace celosia::native;
    DataMemory memory;
    R5900Context ctx;
    auto reg=[&](unsigned index,uint32_t value) { ctx.r[index]=_mm_set_epi64x(0,value); };
    auto text=[&](uint32_t address,const std::string &value) { require(memory.write(address,value.c_str(),value.size()+1),"test string write"); };
    std::string received;
    unsigned calls=0;
    auto sink=[&](std::string_view value) { ++calls; received.assign(value); return static_cast<int>(value.size()); };
    auto run=[&](const std::string &format,bool reentrant=false) {
        received.clear(); calls=0; text(0x1000,format); reg(reentrant?5:4,0x1000);
        return nativePrintf(memory,ctx,reentrant,sink);
    };
    auto rejects=[&](const std::string &format) {
        bool rejected=false;
        try { (void)run(format); } catch(const std::exception &) { rejected=true; }
        require(rejected && calls==0,"format rejected before sink side effects");
    };
    text(0x1100,"loader 100% ready");
    require(nativeWriteText(memory,0x1100,sink)==17 && received=="loader 100% ready",
            "plain loader text never interprets percent conversions");
    reg(5,3); require(run("ld: start: argc:%d \n")==19 && received=="ld: start: argc:3 \n","actual SCUS argc format");
    text(0x2000,"native"); reg(5,1); reg(6,0x2000);
    require(run("ld: \targc %d: %s\n")==20 && received=="ld: \targc 1: native\n","actual SCUS argv format");
    reg(5,0x80000000); reg(6,UINT32_MAX); reg(7,UINT32_MAX);
    require(run("%d %i %u")>0 && received=="-2147483648 -1 4294967295","signed and unsigned widths");
    reg(5,42); reg(6,42); reg(7,42);
    require(run("%#08x %#X %#o")>0 && received=="0x00002a 0X2A 052","integer alternate prefixes");
    reg(5,42); reg(6,42); reg(7,0x2000);
    require(run("%+06d % 5d %-8.3s")>0 && received=="+00042    42 nat     ","flags and precision");
    reg(5,0); reg(6,0); reg(7,0);
    require(run("[%.0d][%#.0o][%#.0x]")>0 && received=="[][0][]","zero precision");
    reg(5,0x1234ABCD); reg(6,'Q');
    require(run("%p %c %%")>0 && received=="0x1234abcd Q %","pointer char percent");
    reg(5,0x80); reg(6,0xFFFF8000); reg(7,0x12345678);
    require(run("%hhd %hd %lx")>0 && received=="-128 -32768 12345678","32bit long and narrow integer modifiers");
    reg(5,7); reg(6,8); require(run("%zu %td")>0 && received=="7 8","32bit size and ptrdiff");
    reg(5,0xDEADBEEF); reg(6,23); reg(7,0x2000);
    require(run("%d:%s",true)==9 && received=="23:native","reentrant ABI shifts fixed arguments");
    reg(29,0x3000); for(unsigned i=5;i<=11;++i) reg(i,i-4);
    const std::array<uint32_t,2> stack{8,9}; require(memory.write(0x3010,stack.data(),sizeof(stack)),"stack test data");
    require(run("%d%d%d%d%d%d%d%d%d")==9 && received=="123456789","register to stack ABI transition");
    reg(5,static_cast<uint32_t>(-8)); reg(6,3); reg(7,0x2000);
    require(run("%*.*s")==8 && received=="nat     ","star width and precision");
    reg(5,static_cast<uint32_t>(-1)); reg(6,0x2000);
    require(run("%.*s")==6 && received=="native","negative precision means absent");
    reg(5,0xFFFFFFFF); require(run("%.0s")==0 && calls==1,"zero string precision reads no bytes");
    const char raw='x'; require(memory.write(0x70003FFF,&raw,1),"boundary string test setup");
    reg(5,0x70003FFF); require(run("%.1s")==1 && received=="x","precision limited nonterminated string");
    rejects("prefix %n"); rejects("prefix %q"); rejects("prefix %f"); rejects("prefix %lld");
    rejects("prefix %ls"); rejects("prefix %"); rejects("%1$d"); rejects("%999999999999999999d");
    reg(5,0x80000000); rejects("%*d");
    reg(5,0xFFFFFFFF); rejects("prefix %s");
    reg(5,0x70003FFF); rejects("prefix %s");
    reg(5,0); rejects("%s");
    reg(5,0x2000); rejects("%.999999999999s");
    reg(5,1); rejects("%4097d");
    rejects(std::string(PrintfFormatLimit,'x'));
    text(0x2000,std::string(PrintfStringLimit,'x')); reg(5,0x2000); rejects("%s");
    reg(5,0); reg(6,0); reg(7,0); reg(8,0); reg(9,0);
    rejects("%4096d%4096d%4096d%4096d%4096d");
    reg(29,UINT32_MAX-4); rejects("%d%d%d%d%d%d%d%d");
    text(0x1000,"sink"); reg(4,0x1000);
    require(nativePrintf(memory,ctx,false,[](std::string_view){return -1;})==-1,"sink failure propagated");
    require(nativePrintf(memory,ctx,false,[](std::string_view){return 1;})==-1,"partial sink output reports error");
    reg(4,0x12000000); bool rejected=false;
    try { nativePrintf(memory,ctx,false,sink); } catch(const std::exception &) { rejected=true; }
    require(rejected,"MMIO format rejected");
    text(0x1000,"%s %08x %d"); text(0x2000,"native");
    const std::array<uint64_t,3> va{0x2000,0x2a,uint64_t(UINT32_MAX)};
    require(memory.write(0x4000,va.data(),sizeof(va)),"eight-byte SCUS variadic spill slots");
    reg(4,0x80005000);reg(5,0x1000);reg(6,0x4000);
    require(nativeSprintf(memory,ctx,true)==18 && std::string(reinterpret_cast<char*>(memory.mainRam().data()+0x5000))=="native 0000002a -1",
            "vsprintf consumes low32 words with verified eight-byte va_list stride");
    text(0x1000,"%d:%s");reg(4,0x5000);reg(5,0x1000);reg(6,7);reg(7,0x2000);
    require(nativeSprintf(memory,ctx)==8 && std::string(reinterpret_cast<char*>(memory.mainRam().data()+0x5000))=="7:native",
            "sprintf begins register arguments at a2");
    text(0x1000,"literal");reg(4,0x1000);reg(5,0x1000);reg(6,0);
    require(nativeSprintf(memory,ctx,true)==7 && std::string(reinterpret_cast<char*>(memory.mainRam().data()+0x1000))=="literal",
            "format/destination overlap renders before write and literal needs no va_list");
    text(0x1000,"");reg(4,DataMemory::MainBytes-1);reg(5,0x1000);
    require(nativeSprintf(memory,ctx,true)==0 && memory.mainRam().back()==0,"empty output writes its terminator at final byte");
    auto rejectedOutput=[&](uint32_t destination,uint32_t arguments,const std::string &format) {
        text(0x1000,format);reg(4,destination);reg(5,0x1000);reg(6,arguments);
        memory.mainRam().back()=0x5a;memory.mainRam()[0x5000]=0xa5;
        bool failed=false;try{(void)nativeSprintf(memory,ctx,true);}catch(const std::exception &){failed=true;}
        require(failed && memory.mainRam().back()==0x5a && memory.mainRam()[0x5000]==0xa5,
                "invalid sprintf call cannot partially mutate destination");
    };
    rejectedOutput(DataMemory::MainBytes-1,0,"x");
    rejectedOutput(0,0,"literal");rejectedOutput(0x12000000,0,"literal");
    rejectedOutput(0x5000,DataMemory::MainBytes-2,"%d");
    rejectedOutput(0x5000,UINT32_MAX-2,"%d%d");
    rejectedOutput(0x5000,0,"%d");rejectedOutput(0x5000,0x4000,"prefix%n");
    rejectedOutput(0x5000,0x4000,"%f");
    const std::array<uint64_t,2> invalidStringVa{0x2000,0x12000000};
    require(memory.write(0x4000,invalidStringVa.data(),sizeof(invalidStringVa)),"invalid second string setup");
    rejectedOutput(0x5000,0x4000,"%s%s");
        std::printf("native printf: %u checks passed\n",checks);
}
