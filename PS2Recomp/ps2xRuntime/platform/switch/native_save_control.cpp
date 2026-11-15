#include "native_save_control.hpp"
#include "native_aot_context.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <limits>
#include <vector>
namespace celosia::native {
namespace {
bool mainRange(uint32_t address,size_t bytes)
{
    if(!address)return false;
    for(const auto base:{0u,0x20000000u,0x80000000u,0xA0000000u}) {
        if(address<base)continue;
        const auto offset=address-base;
        if(offset<=DataMemory::MainBytes && bytes<=DataMemory::MainBytes-offset)return true;
    }
    return false;
}
bool matches(std::string_view pattern,std::string_view name)
{
    size_t at=0,index=0,star=std::string_view::npos,retry=0;
    while(index<name.size()) {
        if(at<pattern.size() && (pattern[at]=='?' || pattern[at]==name[index])) {++at;++index;}
        else if(at<pattern.size() && pattern[at]=='*') {star=at++;retry=index;}
        else if(star!=std::string_view::npos) {at=star+1;index=++retry;}
        else return false;
    }
    while(at<pattern.size() && pattern[at]=='*')++at;
    return at==pattern.size();
}
void date(std::optional<int64_t> seconds,uint8_t *output)
{
    if(!seconds)return;
    const auto value=static_cast<std::time_t>(*seconds);
    if(static_cast<int64_t>(value)!=*seconds)return;
    std::tm calendar{};
#ifdef _WIN32
    if(localtime_s(&calendar,&value)!=0)return;
#else
    if(!localtime_r(&value,&calendar))return;
#endif
    const int64_t year=int64_t(calendar.tm_year)+1900;
    if(year<0 || year>UINT16_MAX)return;
    output[1]=uint8_t(calendar.tm_sec);output[2]=uint8_t(calendar.tm_min);output[3]=uint8_t(calendar.tm_hour);
    output[4]=uint8_t(calendar.tm_mday);output[5]=uint8_t(calendar.tm_mon+1);
    output[6]=uint8_t(year);output[7]=uint8_t(year>>8);
}
std::optional<std::array<uint8_t,0x38>> record(const NativeSaveStore::Metadata &entry)
{
    if(entry.name.empty() || entry.name.size()>=32 || entry.size>UINT32_MAX)return {};
    std::array<uint8_t,0x38> bytes{};
    date(entry.createdUnixSeconds,bytes.data());
    // Native fsdev directory metadata uses zero when modification time is
    // unavailable. Preserve the empty Source field instead of inventing1970.
    if(entry.modifiedUnixSeconds!=0)date(entry.modifiedUnixSeconds,bytes.data()+8);
    const auto size=static_cast<uint32_t>(entry.size);
    const uint16_t attributes=0x8000|(entry.directory?0x20:0x10)|(entry.readable?1:0)|(entry.writable?2:0);
    std::memcpy(bytes.data()+0x10,&size,4);std::memcpy(bytes.data()+0x14,&attributes,2);
    std::memcpy(bytes.data()+0x18,entry.name.data(),entry.name.size());return bytes;
}
}
std::optional<std::string> NativeSaveControl::path(uint32_t address,bool wildcard) const
{
    std::string raw;
    for(uint32_t index=0;index<128;++index) {
        const auto at=uint64_t(address)+index;
        if(at>UINT32_MAX || !mainRange(static_cast<uint32_t>(at),1))return {};
        char value;if(!data.read(static_cast<uint32_t>(at),&value,1))return {};
        if(!value) {
            if(raw.empty())return {};
            std::string result=raw.front()=='/'?raw.substr(1):directory.empty()?raw:directory+"/"+raw;
            while(!result.empty() && result.back()=='/')result.pop_back();
            if(result.size()>=128)return {};
            for(size_t begin=0;begin<result.size();) {
                const auto end=result.find('/',begin),finish=end==std::string::npos?result.size():end;
                const auto component=std::string_view(result).substr(begin,finish-begin);
                if(component.empty() || component=="." || component=="..")return {};
                if(component.find_first_of("*?")!=std::string_view::npos && (!wildcard || end!=std::string::npos))return {};
                begin=finish+1;
            }
            return result;
        }
        const auto byte=static_cast<unsigned char>(value);
        if(byte<32 || byte>=127 || value=='\\' || value==':')return {};
        raw.push_back(value);
    }
    return {};
}
NativeSaveControl::Error NativeSaveControl::info(uint32_t output)
{
    if(!mainRange(output,4))return Error::Generic;
    const auto available=store.freeBytes();if(!available)return available.error;
    const auto units=static_cast<uint32_t>(std::min<uint64_t>(available.value/1024,UINT32_MAX));
    return data.write(output,&units,4)?Error::Success:Error::Generic;
}
NativeSaveControl::Error NativeSaveControl::readFile(const std::string &name,uint32_t destination,uint32_t offset,uint32_t count)
{
    if(offset>INT32_MAX || count>INT32_MAX || uint64_t(offset)+count>INT32_MAX)return Error::Generic;
    const auto metadata=store.stat(name);if(!metadata)return metadata.error;
    if(metadata.value.directory || offset>metadata.value.size)return Error::Generic;
    const auto remaining=metadata.value.size-offset;
    const auto bytes=count?std::min<uint64_t>(count,remaining):remaining;
    if(bytes>MaxPayload || uint64_t(offset)+bytes>INT32_MAX ||
       (bytes && !mainRange(destination,static_cast<size_t>(bytes))))return Error::Generic;
    std::vector<uint8_t> staged(static_cast<size_t>(bytes));
    const auto result=store.readAt(name,offset,staged);if(!result)return result.error;
    if(result.value>staged.size())return Error::Generic;
    if(result.value && !data.write(destination,staged.data(),result.value))return Error::Generic;
    return Error::Success;
}
NativeSaveControl::Error NativeSaveControl::writeFile(const std::string &name,uint32_t source,uint32_t offset,uint32_t count)
{
    if(offset>INT32_MAX || count>MaxPayload || uint64_t(offset)+count>MaxPayload ||
       (count && !mainRange(source,count)))return Error::Generic;
    std::vector<uint8_t> staged(count);
    if(count && !data.read(source,staged.data(),count))return Error::Generic;
    const auto result=store.writeAt(name,offset,staged);
    if(!result)return result.error;
    return result.value==count?Error::Success:Error::StorageBroken;
}
NativeSaveControl::Error NativeSaveControl::list(const std::string &pattern,const R5900Context &context)
{
    const auto skip=getRegU32(&context,7),maximum=getRegU32(&context,8);
    const auto output=getRegU32(&context,9),outCount=getRegU32(&context,10);
    const bool writeRecords=output && maximum;
    if(skip>INT32_MAX || maximum>INT32_MAX || (outCount && !mainRange(outCount,4)) ||
       (writeRecords && (maximum>NativeSaveStore::DirectoryLimit || !mainRange(output,size_t(maximum)*0x38))))return Error::Generic;
    const auto slash=pattern.rfind('/');
    const auto parent=slash==std::string::npos?std::string{}:pattern.substr(0,slash);
    const auto glob=slash==std::string::npos?pattern:pattern.substr(slash+1);
    const auto listing=store.listDirectory(parent);if(!listing)return listing.error;
    auto entries=listing.value;
    std::sort(entries.begin(),entries.end(),[](const auto &left,const auto &right){return left.name<right.name;});
    std::vector<std::array<uint8_t,0x38>> records;
    uint32_t skipped=0,count=0;
    for(const auto &entry:entries) {
        if(!matches(glob,entry.name))continue;
        if(skipped<skip){++skipped;continue;}
        if(maximum && count==maximum)break;
        ++count;
        if(writeRecords){const auto packed=record(entry);if(!packed)return Error::Generic;records.push_back(*packed);}
    }
    if(!records.empty() && !data.write(output,records.data(),records.size()*0x38))return Error::Generic;
    if(outCount && !data.write(outCount,&count,4))return Error::Generic;
    return Error::Success;
}
NativeSaveControl::Error NativeSaveControl::operate(uint32_t address,const R5900Context &context)
{
    if(!initialized || getRegU32(&context,4)!=0 || getRegU32(&context,5)!=0)return Error::Unavailable;
    if(address==0x1B07F0)return info(getRegU32(&context,6));
    const auto name=path(getRegU32(&context,6),address==0x1B0E38);
    if(!name)return Error::Generic;
    switch(address) {
    case 0x1B0918:{const auto entry=store.stat(*name);if(!entry)return entry.error;
        if(!entry.value.directory)return Error::Generic;directory=*name;return Error::Success;}
    case 0x1B09C0:return store.mkdir(*name);
    case 0x1B0A68:return store.createFile(*name);
    case 0x1B0B10:return store.removeFile(*name);
    case 0x1B0BB8:return readFile(*name,getRegU32(&context,7),getRegU32(&context,8),getRegU32(&context,9));
    case 0x1B0CB0:return writeFile(*name,getRegU32(&context,7),getRegU32(&context,8),getRegU32(&context,9));
    case 0x1B0E38:return list(*name,context);
    default:return Error::Generic;
    }
}
bool NativeSaveControl::handle(uint32_t address,R5900Context &context)
{
    Error result=Error::Generic;
    switch(address) {
    case 0x1B06F8:directory.clear();result=store.initialize();initialized=result==Error::Success;break;
    case 0x1B0F20:directory.clear();initialized=false;result=Error::Success;break;
    case 0x1B0DA8:case 0x1B0DF0:
        // These operations target physical-card formatting. Native application
        // files have no such endpoint; never erase them or claim formatting.
        setReturnU32(&context,0x81010013);context.pc=getRegU32(&context,31);return true;
    case 0x1B07F0:case 0x1B0918:case 0x1B09C0:case 0x1B0A68:
    case 0x1B0B10:case 0x1B0BB8:case 0x1B0CB0:case 0x1B0E38:result=operate(address,context);break;
    default:return false;
    }
    setReturnS32(&context,static_cast<int32_t>(result));context.pc=getRegU32(&context,31);return true;
}
}
