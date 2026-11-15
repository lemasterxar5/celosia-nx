#include "native_faults.hpp"
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace celosia::native {
namespace {
class FaultWriter final {
public:
    explicit FaultWriter(std::span<char> destination) noexcept : output(destination) {}
    void append(const char *text) noexcept { while(*text) append(*text++); }
    void hex(uint64_t value,unsigned digits) noexcept
    {
        append("0x");
        for(unsigned i=digits;i>0;--i)
            append("0123456789abcdef"[(value>>((i-1)*4))&15]);
    }
    std::size_t finish() noexcept
    {
        if(!output.empty()) output[written]='\0';
        return written;
    }
private:
    void append(char value) noexcept
    {
        if(!output.empty() && written<output.size()-1) output[written++]=value;
    }
    std::span<char> output;
    std::size_t written=0;
};
}
std::size_t formatNativeFault(const NativeFaultInfo &fault,std::span<char> output) noexcept
{
    FaultWriter writer(output);
    writer.append("Native ARM64 fatal fault: description="); writer.hex(fault.description,8);
    writer.append(" esr="); writer.hex(fault.esr,8);
    writer.append(" pc="); writer.hex(fault.pc,16);
    writer.append(" lr="); writer.hex(fault.lr,16);
    writer.append(" sp="); writer.hex(fault.sp,16);
    writer.append(" far="); writer.hex(fault.far,16);
    writer.append("\n");
    return writer.finish();
}
bool NativeFaults::initialize() noexcept
{
#ifdef __SWITCH__
    ready=true;
#endif
    return ready;
}
}

#ifdef __SWITCH__
// libnx's exception entry calls this strong symbol on its exception stack.
// Returning here enters libnx's svcBreak fatal path, never the faulting game code.
extern "C" void switchFaultRecord(const char *,size_t) __attribute__((weak));
extern "C" void __libnx_exception_handler(ThreadExceptionDump *dump)
{
    if(!dump) {
        static constexpr char message[]="Native ARM64 fatal fault: missing exception dump\n";
        svcOutputDebugString(message,sizeof(message)-1);
        return;
    }
    const celosia::native::NativeFaultInfo fault{
        dump->pc.x,dump->lr.x,dump->sp.x,dump->far.x,dump->esr,dump->error_desc};
    char output[256];
    const auto length=celosia::native::formatNativeFault(fault,output);
    if(switchFaultRecord)switchFaultRecord(output,length);
    // Best effort only: logging failure must not interrupt libnx's fatal path.
    (void)svcOutputDebugString(output,length);
}
#endif
