#include <switch.h>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <atomic>
#include <algorithm>
#include <cstring>
namespace {
alignas(16) unsigned char exceptionStack[0x4000];
FsFile faultFile{};
std::atomic<bool> faultFileReady{false};
std::atomic_flag faultWritten=ATOMIC_FLAG_INIT;
}
extern "C" {
extern unsigned char _start[];
void *__nx_exception_stack=exceptionStack;
u64 __nx_exception_stack_size=sizeof(exceptionStack);
}
void prepareSwitchDiagnostics(){
    auto *filesystem=fsdevGetDeviceFileSystem("sdmc");
    if(!filesystem)return;
    constexpr const char *path="/switch/celosia_nx/native-fault.txt";
    (void)fsFsCreateFile(filesystem,path,4096,0);
    if(R_FAILED(fsFsOpenFile(filesystem,path,FsOpenMode_Write,&faultFile)))return;
    faultFileReady=true;
    // A preopened file avoids FILE locks and allocations on the fatal path.
    char initial[4096]="armed; no native fatal recorded in this run\n";
    (void)fsFileSetSize(&faultFile,sizeof(initial));
    (void)fsFileWrite(&faultFile,0,initial,sizeof(initial),FsWriteOption_Flush);
}
extern "C" void switchAsyncLogFlushNow();
extern "C" void switchFaultRecord(const char *message,size_t size){
    switchAsyncLogFlushNow(); // v169: the log lines still in memory first (try-locks only)
    if(!faultFileReady || faultWritten.test_and_set())return;
    size=size>4095?4095:size;
    (void)fsFileWrite(&faultFile,0,message,size,FsWriteOption_Flush);
    char metadata[128];u64 threadId=0;(void)svcGetThreadId(&threadId,CUR_THREAD_HANDLE);
    const int count=std::snprintf(metadata,sizeof(metadata),"nro_base=0x%llx thread_id=0x%llx\n",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_start)),static_cast<unsigned long long>(threadId));
    const auto extra=count>0?std::min(static_cast<size_t>(count),sizeof(metadata)-1):0;
    if(extra)(void)fsFileWrite(&faultFile,size,metadata,extra,FsWriteOption_Flush);
    (void)fsFileSetSize(&faultFile,size+extra);
    (void)fsFileFlush(&faultFile);
    fsFileClose(&faultFile);faultFileReady=false;
}
namespace {
void windowEvent(const char *operation,NWindow *window,void *caller)
{
    char message[192];
    const auto offset=reinterpret_cast<uintptr_t>(caller)-reinterpret_cast<uintptr_t>(_start);
    const int count=std::snprintf(message,sizeof(message),"WINDOW %s configured=0x%llx requested=0x%llx caller-offset=0x%llx\n",operation,
        static_cast<unsigned long long>(window?window->slots_configured:0),
        static_cast<unsigned long long>(window?window->slots_requested:0),static_cast<unsigned long long>(offset));
    if(count>0)svcOutputDebugString(message,static_cast<size_t>(count));
    auto frame=reinterpret_cast<uintptr_t>(__builtin_frame_address(0));MemoryInfo memory{};u32 page=0;
    if(R_SUCCEEDED(svcQueryMemory(&memory,&page,frame)) && (memory.perm&Perm_R)) {
        const auto end=memory.addr+memory.size;
        for(unsigned index=0;index<10 && !(frame&15) && frame>=memory.addr && frame<=end && end-frame>=16;++index) {
            const auto *record=reinterpret_cast<const uintptr_t *>(frame);
            const int size=std::snprintf(message,sizeof(message),"window-backtrace[%u] offset=0x%llx\n",index,
                static_cast<unsigned long long>(record[1]-reinterpret_cast<uintptr_t>(_start)));
            if(size>0)svcOutputDebugString(message,static_cast<size_t>(size));
            const auto next=record[0];if(next<=frame)break;frame=next;
        }
    }
}
}
void recordSwitchTermination(){
    char message[1024];const char *reason="no active C++ exception";
    try{if(auto error=std::current_exception())std::rethrow_exception(error);}
    catch(const std::exception &error){reason=error.what();}
    catch(...){reason="unknown C++ exception";}
    const int count=std::snprintf(message,sizeof(message),"TERMINATE: %s caller-offset=0x%llx\n",reason,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))-reinterpret_cast<uintptr_t>(_start)));
    // v86: the terminating thread's frame-record chain (offsets from _start), to name the caller of
    // std::terminate from the shipped ELF.
    size_t used=count>0?std::min(static_cast<size_t>(count),sizeof(message)-1):0;
    auto frame=reinterpret_cast<uintptr_t>(__builtin_frame_address(0));MemoryInfo memory{};u32 page=0;
    if(used && R_SUCCEEDED(svcQueryMemory(&memory,&page,frame)) && (memory.perm&Perm_R)) {
        const auto end=memory.addr+memory.size;
        for(unsigned index=0;index<24 && !(frame&15) && frame>=memory.addr && frame<=end && end-frame>=16 && sizeof(message)-used>40;++index) {
            const auto *record=reinterpret_cast<const uintptr_t *>(frame);
            const int size=std::snprintf(message+used,sizeof(message)-used,"bt[%u]=0x%llx\n",index,
                static_cast<unsigned long long>(record[1]-reinterpret_cast<uintptr_t>(_start)));
            if(size>0)used+=static_cast<size_t>(size);
            const auto next=record[0];if(next<=frame)break;frame=next;
        }
    }
    if(used)switchFaultRecord(message,std::min(used,sizeof(message)-1));
    windowEvent("terminate",nullptr,__builtin_return_address(0));
}
namespace {
std::atomic<uint64_t> mapCreates{0},mapCloses{0};
thread_local u32 mapRequestBytes=0;
void recordMapFailure(const char *operation,Result result,u32 handle,NvKind kind){
    char message[384];
    const int count=std::snprintf(message,sizeof(message),
        "NVIDIA %s result=0x%x module=%u description=%u size=%u handle=%u kind=%u creates=%llu closes=%llu\n",
        operation,result,R_MODULE(result),R_DESCRIPTION(result),mapRequestBytes,handle,unsigned(kind),
        static_cast<unsigned long long>(mapCreates.load()),static_cast<unsigned long long>(mapCloses.load()));
    if(count>0)switchFaultRecord(message,std::min(static_cast<size_t>(count),sizeof(message)-1));
}
}
extern "C" Result __real_nvMapCreate(NvMap *,void *,u32,u32,NvKind,bool);
extern "C" Result __wrap_nvMapCreate(NvMap *map,void *address,u32 size,u32 alignment,NvKind kind,bool cached){
    mapRequestBytes=size;
    const Result result=__real_nvMapCreate(map,address,size,alignment,kind,cached);
    if(R_FAILED(result))recordMapFailure("nvMapCreate",result,0,kind);
    else ++mapCreates;
    return result;
}
extern "C" void __real_nvMapClose(NvMap *);
extern "C" void __wrap_nvMapClose(NvMap *map){
    if(map && map->has_init)++mapCloses;
    __real_nvMapClose(map);
}
extern "C" Result __real_nvAddressSpaceMap(NvAddressSpace *,u32,bool,NvKind,iova_t *);
extern "C" Result __wrap_nvAddressSpaceMap(NvAddressSpace *space,u32 handle,bool cached,NvKind kind,iova_t *address){
    const Result result=__real_nvAddressSpaceMap(space,handle,cached,kind,address);
    if(R_FAILED(result))recordMapFailure("nvAddressSpaceMap",result,handle,kind);
    return result;
}
extern "C" Result __real_nwindowReleaseBuffers(NWindow *);
extern "C" void __real_nwindowClose(NWindow *);
extern "C" Result __wrap_nwindowReleaseBuffers(NWindow *window)
{windowEvent("release",window,__builtin_return_address(0));return __real_nwindowReleaseBuffers(window);}
extern "C" void __wrap_nwindowClose(NWindow *window)
{windowEvent("close",window,__builtin_return_address(0));__real_nwindowClose(window);}
extern "C" [[noreturn]] void __real_abort();
extern "C" [[noreturn]] void __real_exit(int);
extern "C" [[noreturn]] void __real__Exit(int);
extern "C" [[noreturn]] void __real__exit(int);
extern "C" [[noreturn]] void __real___libnx_exit();
namespace {
void processExit(const char *kind,int code,void *caller)
{
    char message[160];const int count=std::snprintf(message,sizeof(message),"PROCESS %s code=%d caller-offset=0x%llx\n",kind,code,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(caller)-reinterpret_cast<uintptr_t>(_start)));
    if(count>0)svcOutputDebugString(message,static_cast<size_t>(count));
}
}
extern "C" [[noreturn]] void __wrap_exit(int code)
{processExit("exit",code,__builtin_return_address(0));__real_exit(code);}
extern "C" [[noreturn]] void __wrap__Exit(int code)
{processExit("_Exit",code,__builtin_return_address(0));__real__Exit(code);}
extern "C" [[noreturn]] void __wrap__exit(int code)
{processExit("_exit",code,__builtin_return_address(0));__real__exit(code);}
extern "C" [[noreturn]] void __wrap___libnx_exit()
{processExit("libnx-exit",0,__builtin_return_address(0));__real___libnx_exit();}
extern "C" [[noreturn]] void __wrap_abort()
{
    char message[320];
    const char *reason="no active C++ exception";
    try{if(auto error=std::current_exception())std::rethrow_exception(error);}
    catch(const std::exception &error){reason=error.what();
        const int count=std::snprintf(message,sizeof(message),"NATIVE ABORT exception: %s\n",reason);
        if(count>0)svcOutputDebugString(message,static_cast<size_t>(count)<sizeof(message)?count:sizeof(message)-1);
        __real_abort();}
    catch(...){reason="unknown C++ exception";}
    const int count=std::snprintf(message,sizeof(message),"NATIVE ABORT: %s caller-offset=0x%llx\n",reason,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))-reinterpret_cast<uintptr_t>(_start)));
    if(count>0)svcOutputDebugString(message,static_cast<size_t>(count)<sizeof(message)?count:sizeof(message)-1);
    __real_abort();
}
extern "C" void diagAbortWithResult(Result result)
{
    char message[192];
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const auto offset=caller-reinterpret_cast<uintptr_t>(_start);
    const int length=std::snprintf(message,sizeof(message),"LIBNX ABORT result=0x%08x module=%u description=%u caller-offset=0x%llx\n",
        result,R_MODULE(result),R_DESCRIPTION(result),static_cast<unsigned long long>(offset));
    if(length>0)switchFaultRecord(message,std::min(static_cast<size_t>(length),sizeof(message)-1));
    // A fatal GPU callback can hold stdio locks. Kernel debug output remains
    // usable there; do not block the fault path trying to acquire FILE locks.
    if(length>0)svcOutputDebugString(message,static_cast<size_t>(length));
    auto frame=reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    MemoryInfo memory{};u32 page=0;
    if(R_SUCCEEDED(svcQueryMemory(&memory,&page,frame)) && (memory.perm&Perm_R)) {
        const auto end=memory.addr+memory.size;
        for(unsigned index=0;index<8 && !(frame&15) && frame>=memory.addr && frame<=end && end-frame>=16;++index) {
            const auto *record=reinterpret_cast<const uintptr_t *>(frame);
            const auto returnOffset=record[1]-reinterpret_cast<uintptr_t>(_start);
            const int count=std::snprintf(message,sizeof(message),"native-backtrace[%u] offset=0x%llx\n",index,static_cast<unsigned long long>(returnOffset));
            if(count>0)svcOutputDebugString(message,static_cast<size_t>(count));
            const auto next=record[0];if(next<=frame)break;frame=next;
        }
    }
    svcBreak(0,reinterpret_cast<uintptr_t>(&result),sizeof(result));
    __builtin_trap();
}
