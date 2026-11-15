#include "native_file_api.hpp"
#include "native_files.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    using namespace celosia::native;
    const auto root=std::filesystem::absolute("native-file-api-test-fixture");
    require(!std::filesystem::exists(root),"fixture exclusively owned");
    std::filesystem::create_directory(root);
    struct Cleanup {std::filesystem::path root;~Cleanup(){std::filesystem::remove_all(root);}} cleanup{root};
    std::vector<uint8_t> input(32785);
    for(size_t i=0;i<input.size();++i) input[i]=static_cast<uint8_t>(i%251);
    {std::ofstream out(root/"DATA.BIN",std::ios::binary);out.write(reinterpret_cast<const char *>(input.data()),input.size());}
    {std::ofstream out(root/"EMPTY.BIN",std::ios::binary);}
    NativeFiles files(root.generic_string()); DataMemory memory;
    std::array<uint64_t,19> cached{};
    for(auto &token:cached) {token=files.open("DATA.BIN");require(token!=0,"external asset cache token");}
    NativeFileApi api(files,memory);
    require(api.open("../DATA.BIN")==-1 && api.open("MISSING.BIN")==-1,"invalid paths and missing file fail");
    require(api.close(-1)==-1 && !api.size(-1) && !api.seek(-1,0,0),"invalid descriptor operations fail");
    require(api.read(-1,0,0)==-1,"zero read still requires valid descriptor");
    const auto first=api.open("cdrom0:\\DATA.BIN;1");
    require(first>=3,"asset descriptors reserve native stdin/stdout/stderr identities");
    require(first>0 && api.size(first)==input.size(),"native source file opened with actual size");
    require(api.read(first,UINT32_MAX,0)==0 && api.seek(first,0,1)==0,"zero read does not dereference invalid destination");
    require(api.read(first,0x80001000,7)==7,"aliased main data destination");
    require(std::equal(input.begin(),input.begin()+7,memory.mainRam().begin()+0x1000),"real file bytes copied");
    require(api.seek(first,0,1)==7,"read advances native cursor");
    require(api.seek(first,-2,1)==5 && api.read(first,0x2000,2)==2 && memory.mainRam()[0x2000]==input[5],"signed relative seek");
    require(api.seek(first,-3,2)==input.size()-3,"signed EOF-relative seek");
    require(api.read(first,0x3000,16)==3 && api.seek(first,0,1)==input.size(),"EOF short read returns actual count and cursor");
    require(api.read(first,0x3000,16)==0,"EOF read is zero");
    require(api.seek(first,100,2)==input.size()+100 && api.read(first,0x3000,4)==0,"seeking beyond EOF does not fabricate bytes");
    require(!api.seek(first,-1,0) && !api.seek(first,INT64_MIN,1),"negative cursor and signed underflow fail");
    require(!api.seek(first,INT64_MAX,2),"EOF addition overflow rejected");
    require(api.seek(first,INT64_MAX,0)==uint64_t(INT64_MAX),"maximum representable cursor accepted");
    require(!api.seek(first,1,1) && api.seek(first,0,1)==uint64_t(INT64_MAX),"overflow leaves cursor unchanged");
    require(!api.seek(first,0,3),"unknown seek origin rejected");
    require(api.seek(first,0,0)==0,"cursor reset");
    memory.mainRam()[0x1FFFFFF]=0xA5;
    for(const auto address:{0x1FFFFFFu,0x70000000u,0x11004000u,0x12000000u,0xFFFFFFF0u}) {
        require(api.read(first,address,32)==-1,"whole destination must be bounded main data");
        require(api.seek(first,0,1)==0 && memory.mainRam()[0x1FFFFFF]==0xA5,"invalid read leaves cursor/data unchanged");
    }
    require(api.read(first,0,UINT32_MAX)==-1,"oversized range fails before I/O");
    require(api.read(first,0xA0010000,65536)==int32_t(input.size()),"multi-chunk read returns partial EOF count");
    require(std::equal(input.begin(),input.end(),memory.mainRam().begin()+0x10000),"all 16KiB chunks copied in order");
    require(api.seek(first,0,1)==input.size(),"multi-chunk cursor tracks copied data");
    require(api.close(first)==0 && api.close(first)==-1,"descriptor closes exactly once");
    require(!api.size(first) && !api.seek(first,0,0) && api.read(first,0x1000,1)==-1,"stale descriptor rejected");
    const auto next=api.open("EMPTY.BIN");
    require(next>first && api.size(next)==0 && api.read(next,0x1000,8)==0,"monotonic IDs and empty file");
    require(api.close(next)==0,"empty file close");
    std::array<int32_t,NativeFileApi::MaxLive> descriptors{};
    for(auto &fd:descriptors) {fd=api.open("DATA.BIN");require(fd>0,"bounded native descriptor opens");}
    require(api.open("DATA.BIN")==-1,"native descriptor cap enforced independently of file tokens");
    require(api.reset(),"reset closes own descriptors");
    for(const auto fd:descriptors) require(api.close(fd)==-1,"reset descriptors remain stale");
    for(const auto token:cached) require(files.size(token)==input.size(),"reset preserves external asset tokens");
    const auto afterReset=api.open("DATA.BIN");
    require(afterReset>descriptors.back(),"reset never reuses descriptor identity");
    require(api.reset() && api.reset(),"empty reset idempotent");
    for(const auto token:cached) require(files.close(token),"external tokens still owned by caller");
    std::array<int32_t,8> concurrent{}; std::vector<std::thread> workers;
    for(unsigned i=0;i<concurrent.size();++i) workers.emplace_back([&,i]{concurrent[i]=api.open("DATA.BIN");});
    for(auto &worker:workers) worker.join();
    for(unsigned i=0;i<concurrent.size();++i) {
        require(concurrent[i]>0,"concurrent descriptor open");
        for(unsigned j=0;j<i;++j) require(concurrent[i]!=concurrent[j],"concurrent IDs unique");
    }
    require(api.reset(),"concurrent descriptors closed");
    std::printf("native file API: %u checks passed\n",checks);
}
