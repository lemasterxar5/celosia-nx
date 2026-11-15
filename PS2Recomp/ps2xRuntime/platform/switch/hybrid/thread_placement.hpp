#pragma once
#ifdef __SWITCH__
#include <switch.h>
#include <cstdio>
namespace celosia::native {
inline void placeSwitchThread(const char *role,int desired){
    u64 allowed=0,actualMask=0;s32 actualCore=-1;
    const Result queried=svcGetInfo(&allowed,InfoType_CoreMask,CUR_PROCESS_HANDLE,0);
    Result moved=0;
    if(desired>=0 && desired<4 && R_SUCCEEDED(queried) && (allowed&(1ull<<desired)))
        moved=svcSetThreadCoreMask(CUR_THREAD_HANDLE,desired,1u<<desired);
    const Result actual=svcGetThreadCoreMask(&actualCore,&actualMask,CUR_THREAD_HANDLE);
    std::fprintf(stderr,"[thread] %s allowed=0x%llx desired=%d actual=%d mask=0x%llx query=0x%x move=0x%x read=0x%x\n",
        role,static_cast<unsigned long long>(allowed),desired,actualCore,static_cast<unsigned long long>(actualMask),queried,moved,actual);
}
}
#else
namespace celosia::native {inline void placeSwitchThread(const char *,int){}}
#endif
