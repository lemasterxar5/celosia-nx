#include "native_printf.hpp"
#include "native_aot_runtime.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace celosia::native {
namespace {
[[noreturn]] void invalid(const char *why) { throw std::invalid_argument(why); }
std::string readString(const DataMemory &memory,uint32_t address,size_t limit,bool terminated)
{
    if(!limit) return {};
    if(!address) invalid("Native printf: null string");
    std::string result;
    for(size_t i=0;i<limit;++i) {
        char value=0;
        const uint64_t at=uint64_t(address)+i;
        if(at>UINT32_MAX || !memory.read(static_cast<uint32_t>(at),&value,1))
            throw std::out_of_range("Native printf: string exceeds data bounds");
        if(!value) return result;
        result.push_back(value);
    }
    if(terminated) invalid("Native printf: unterminated or oversized string");
    return result;
}
struct Arguments {
    const DataMemory &memory;
    const R5900Context &context;
    uint32_t slot;
    bool useVaList=false;
    uint32_t vaAddress=0;
    uint32_t next()
    {
        if(slot>=64) invalid("Native printf: argument limit");
        const auto current=slot++;
        if(!useVaList && current<8) return getRegU32(&context,4+current);
        // SCUS LoaderSysPrintf (102D00..102D28) spills each variadic GPR
        // with sd, eight bytes apart, and passes the first slot in a2.
        if(useVaList && !vaAddress) invalid("Native sprintf: null va_list");
        const uint64_t address=useVaList?uint64_t(vaAddress)+uint64_t(current)*8:
            uint64_t(getRegU32(&context,29))+16+uint64_t(current-8)*4;
        uint32_t value=0;
        if(address>UINT32_MAX || !memory.read(static_cast<uint32_t>(address),&value,sizeof(value)))
            throw std::out_of_range("Native printf: stack argument exceeds data bounds");
        return value;
    }
};
struct Spec {
    bool left=false,plus=false,space=false,alternate=false,zero=false;
    int width=0,precision=-1;
    unsigned bits=32;
    bool hasLength=false;
    char conversion=0;
};
void bounded(int value)
{ if(value<0 || value>int(PrintfStringLimit)) invalid("Native printf: field exceeds limit"); }
int decimal(std::string_view format,size_t &position)
{
    int value=0;
    while(position<format.size() && format[position]>='0' && format[position]<='9') {
        value=value*10+format[position++]-'0';
        bounded(value);
    }
    return value;
}
Spec parse(std::string_view format,size_t &position,Arguments &arguments)
{
    Spec result;
    while(position<format.size()) {
        const char flag=format[position];
        if(flag=='-') result.left=true;
        else if(flag=='+') result.plus=true;
        else if(flag==' ') result.space=true;
        else if(flag=='#') result.alternate=true;
        else if(flag=='0') result.zero=true;
        else break;
        ++position;
    }
    if(position<format.size() && format[position]=='*') {
        ++position;
        const int64_t width=static_cast<int32_t>(arguments.next());
        result.left=result.left || width<0;
        if(width < -int64_t(PrintfStringLimit) || width>int64_t(PrintfStringLimit))
            invalid("Native printf: field exceeds limit");
        result.width=static_cast<int>(width<0?-width:width);
    } else result.width=decimal(format,position);
    if(position<format.size() && format[position]=='.') {
        ++position;
        if(position<format.size() && format[position]=='*') {
            ++position; result.precision=static_cast<int32_t>(arguments.next());
            if(result.precision<0) result.precision=-1;
            else bounded(result.precision);
        } else result.precision=decimal(format,position);
    }
    if(position<format.size() && std::string_view("hlzt").find(format[position])!=std::string_view::npos) {
        result.hasLength=true;
        const auto length=format[position++];
        if(length=='h') {
            result.bits=16;
            if(position<format.size() && format[position]=='h') { ++position; result.bits=8; }
        }
    }
    if(position==format.size()) invalid("Native printf: incomplete format");
    result.conversion=format[position++];
    return result;
}
void append(std::string &output,std::string_view value)
{
    if(value.size()>PrintfOutputLimit-output.size()) invalid("Native printf: output limit");
    output.append(value);
}
std::string pad(std::string value,const Spec &spec,std::string prefix={},bool numeric=false)
{
    if(spec.precision>=0 && numeric && value.size()<size_t(spec.precision))
        value.insert(0,size_t(spec.precision)-value.size(),'0');
    const size_t used=prefix.size()+value.size();
    const size_t spaces=spec.width>int(used)?size_t(spec.width)-used:0;
    if(numeric && spec.zero && !spec.left && spec.precision<0) return prefix+std::string(spaces,'0')+value;
    value=prefix+value;
    if(spec.left) value.append(spaces,' '); else value.insert(0,spaces,' ');
    return value;
}
std::string integer(uint32_t raw,const Spec &spec)
{
    const bool pointer=spec.conversion=='p';
    const bool sign=spec.conversion=='d' || spec.conversion=='i';
    const uint64_t modulus=uint64_t(1)<<spec.bits;
    const uint64_t value=uint64_t(raw)&(modulus-1);
    const bool negative=sign && value>=(modulus>>1);
    const uint64_t magnitude=negative?modulus-value:value;
    const unsigned base=spec.conversion=='o'?8:spec.conversion=='x' || spec.conversion=='X' || pointer?16:10;
    std::array<char,32> buffer{};
    const auto converted=std::to_chars(buffer.data(),buffer.data()+buffer.size(),magnitude,base);
    if(converted.ec!=std::errc{}) throw std::logic_error("Native printf: integer conversion failed");
    std::string digits(buffer.data(),converted.ptr);
    if(spec.conversion=='X') for(char &c:digits) if(c>='a' && c<='f') c-=32;
    if(!magnitude && spec.precision==0) digits.clear();
    std::string prefix;
    if(negative) prefix="-"; else if(sign && spec.plus) prefix="+"; else if(sign && spec.space) prefix=" ";
    if(pointer || (spec.alternate && magnitude && base==16)) prefix=spec.conversion=='X'?"0X":"0x";
    Spec adjusted=spec;
    if(pointer) adjusted.precision=8;
    if(spec.alternate && base==8 && (digits.empty() || digits[0]!='0') &&
       spec.precision<=int(digits.size())) prefix="0";
    return pad(std::move(digits),adjusted,std::move(prefix),true);
}
std::string conversion(const DataMemory &memory,Arguments &arguments,const Spec &spec)
{
    if(std::string_view("diuoxX").find(spec.conversion)!=std::string_view::npos)
        return integer(arguments.next(),spec);
    if(spec.hasLength) invalid("Native printf: unsupported string/char/pointer length");
    if(spec.conversion=='p') {
        if(spec.precision>=0 || spec.plus || spec.space || spec.alternate)
            invalid("Native printf: unsupported pointer flags");
        return integer(arguments.next(),spec);
    }
    if(spec.conversion!='s' && spec.conversion!='c') invalid("Native printf: unsupported conversion");
    if(spec.plus || spec.space || spec.alternate || spec.zero || (spec.conversion=='c' && spec.precision>=0))
        invalid("Native printf: unsupported text flags");
    const auto value=arguments.next();
    if(spec.conversion=='c') return pad(std::string(1,static_cast<char>(value)),spec);
    const auto limit=spec.precision<0?PrintfStringLimit:size_t(spec.precision);
    return pad(readString(memory,value,limit,spec.precision<0),spec);
}
int writeStdout(std::string_view output)
{
    const auto written=std::fwrite(output.data(),1,output.size(),stdout);
    return written==output.size() && !std::ferror(stdout)?static_cast<int>(written):-1;
}
}
namespace {
std::string render(std::string_view format,const DataMemory &memory,Arguments &arguments)
{
    std::string output;
    for(size_t position=0;position<format.size();) {
        if(format[position]!='%') { append(output,std::string_view(format).substr(position++,1)); continue; }
        ++position;
        if(position<format.size() && format[position]=='%') { ++position; append(output,"%"); continue; }
        append(output,conversion(memory,arguments,parse(format,position,arguments)));
    }
    return output;
}
}
int32_t nativePrintf(DataMemory &memory,R5900Context &context,bool reentrant,PrintSink sink)
{
    const auto format=readString(memory,getRegU32(&context,reentrant?5:4),PrintfFormatLimit,true);
    Arguments arguments{memory,context,reentrant?2u:1u};
    const auto output=render(format,memory,arguments);
    // No sink or stdout mutation occurs until the entire format and arguments
    // have been validated and rendered. Never forward game format to host printf.
    const auto result=sink?sink(output):writeStdout(output);
    return result==int(output.size())?result:-1;
}
int32_t nativeSprintf(DataMemory &memory,R5900Context &context,bool vaList)
{
    const auto destination=getRegU32(&context,4);
    const auto format=readString(memory,getRegU32(&context,5),PrintfFormatLimit,true);
    Arguments arguments{memory,context,vaList?0u:2u,vaList,getRegU32(&context,6)};
    auto output=render(format,memory,arguments);
    const auto bytes=output.size();
    output.push_back('\0');
    // Unlike the prior compatibility provider, never silently truncate output.
    // Validate the complete destination including NUL before any data mutation.
    if(!destination || !memory.contains(destination,output.size()))
        throw std::out_of_range("Native sprintf: destination exceeds data bounds");
    if(!memory.write(destination,output.data(),output.size()))
        throw std::logic_error("Native sprintf: validated destination write failed");
    return static_cast<int32_t>(bytes);
}
int32_t nativeWriteText(DataMemory &memory,uint32_t address,PrintSink sink)
{
    const auto text=readString(memory,address,PrintfStringLimit,true);
    const auto result=sink?sink(text):writeStdout(text);
    return result==int(text.size())?result:-1;
}
}
namespace {
void formatToData(uint8_t *ram,R5900Context *context,PS2Runtime *runtime,bool vaList)
{
    if(!runtime || !context || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native sprintf: invalid data context");
    setReturnS32(context,celosia::native::nativeSprintf(runtime->memory(),*context,vaList));
}
void print(uint8_t *ram,R5900Context *context,PS2Runtime *runtime,bool reentrant)
{
    if(!runtime || !context || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native printf: invalid data context");
    setReturnS32(context,celosia::native::nativePrintf(runtime->memory(),*context,reentrant));
}
}
namespace ps2_stubs {
void sprintf(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { formatToData(ram,context,runtime,false); }
void vsprintf(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { formatToData(ram,context,runtime,true); }
void printf(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { print(ram,context,runtime,false); }
void printf_r(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { print(ram,context,runtime,true); }
}
