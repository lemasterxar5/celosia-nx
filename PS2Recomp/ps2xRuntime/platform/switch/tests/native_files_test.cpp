#include "native_files.hpp"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
static int checkFiles()
{
    using namespace celosia::native;
    const auto base=std::filesystem::absolute("native-files-test-fixture");
    require(!std::filesystem::exists(base),"test fixture is exclusively owned");
    std::filesystem::create_directories(base/"DIR");
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::filesystem::remove_all(path);} } cleanup{base};
    { std::ofstream out(base/"DIR"/"TEST.BIN",std::ios::binary); out<<"abcdefgh"; }
    constexpr uint64_t largeSize=(uint64_t{1}<<31)+17;
#ifdef _WIN32
    const auto sparse=CreateFileW((base/"HUGE.BIN").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,0,nullptr);
    require(sparse!=INVALID_HANDLE_VALUE,"large native asset fixture opens");
    DWORD controlBytes=0;
    const bool marked=DeviceIoControl(sparse,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&controlBytes,nullptr)!=0;
    LARGE_INTEGER end{}; end.QuadPart=largeSize-1;
    DWORD written=0; const char tail='q';
    const bool extended=marked && SetFilePointerEx(sparse,end,nullptr,FILE_BEGIN) && WriteFile(sparse,&tail,1,&written,nullptr);
    CloseHandle(sparse);
    require(extended && written==1,"sparse asset larger than 2GiB created without data allocation");
#else
    { std::ofstream out(base/"HUGE.BIN",std::ios::binary); out.seekp(largeSize-1); out.put('q'); }
#endif
    NativeFiles files(base.generic_string());
    const auto large=files.open("HUGE.BIN");
    std::array<uint8_t,1> tailByte{};
    require(large && files.size(large)==largeSize && files.read(large,largeSize-1,tailByte)==1 && tailByte[0]=='q',
            "large native assets retain 64-bit stat and read offsets");
    require(files.close(large),"large native asset closes");
    const auto full=files.translate("cdrom0:\\dir\\test.bin;1");
    require(full.ends_with("/DIR/TEST.BIN"),"ISO name maps to native file");
    for(const auto path:{"../SECRET","DIR/../TEST.BIN","/DIR/TEST.BIN","host:secret","DIR//TEST.BIN","DIR/./TEST.BIN","cdrom0:\\..\\SECRET","DIR/TEST.BIN;2"}) {
        bool rejected=false; try { (void)files.translate(path); } catch(const std::invalid_argument &) { rejected=true; }
        require(rejected,"invalid path cannot leave configured assets");
    }
    require(files.open(std::string("DIR/TEST.BIN\0evil",17))==0,"embedded null rejected");
    require(files.open("MISSING.BIN")==0 && files.open("DIR")==0,"missing files and directories rejected");
    const auto first=files.open(full);
    require(first>0 && files.size(first)==8,"real readonly file and size");
    std::array<uint8_t,8> output{};
    require(files.read(first,2,std::span(output).first(3))==3 && output[0]=='c' && output[2]=='e',"positioned native read");
    require(files.read(first,6,output)==2 && output[0]=='g' && output[1]=='h',"EOF returns short read");
    require(files.read(first,8,output)==0,"EOF empty read");
    require(!files.read(first,UINT64_MAX,output),"huge offset rejected");
    require(files.close(first) && !files.close(first) && !files.size(first) && !files.read(first,0,output),"stale tokens rejected");
    const auto next=files.open("dir/test.bin");
    require(next>first,"opaque IDs never reuse a stale file token");
    require(files.close(next),"second file closes");
    std::array<uint64_t,NativeFiles::MaxLive> handles{};
    for(auto &handle:handles) { handle=files.open(full); require(handle>0,"bounded file slot opens"); }
    require(files.open(full)==0,"file descriptor cap enforced");
    for(const auto handle:handles) require(files.close(handle),"bounded handles close");
    std::error_code error;
    std::filesystem::create_directory_symlink(base.parent_path(),base/"ESCAPE",error);
    if(!error) require(files.open("ESCAPE/native-files-test-fixture/DIR/TEST.BIN")==0,"symlink outside root rejected");
    else std::puts("SKIP: host lacks permission to create symlink fixture");
    std::printf("native asset files: %u checks passed\n",checks);
    return 0;
}
int main()
{
    try { return checkFiles(); }
    catch(const std::exception &error) { std::fprintf(stderr,"Native files test: %s\n",error.what()); return 1; }
}
