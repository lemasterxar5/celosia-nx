#include "native_save_store.hpp"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
using E=NativeSaveStore::Error;
#ifdef _WIN32
bool junction(const std::filesystem::path &link,const std::filesystem::path &target)
{
    if(!CreateDirectoryW(link.c_str(),nullptr))return false;
    const auto file=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(file==INVALID_HANDLE_VALUE){RemoveDirectoryW(link.c_str());return false;}
    struct Buffer {DWORD tag;WORD length,reserved,substituteOffset,substituteLength,printOffset,printLength;WCHAR names[2048];} buffer{};
    const auto display=target.wstring(),substitute=L"\\??\\"+display;
    if(substitute.size()+display.size()+2>2048){CloseHandle(file);RemoveDirectoryW(link.c_str());return false;}
    buffer.tag=IO_REPARSE_TAG_MOUNT_POINT;buffer.substituteLength=WORD(substitute.size()*2);
    buffer.printOffset=WORD((substitute.size()+1)*2);buffer.printLength=WORD(display.size()*2);
    std::copy(substitute.begin(),substitute.end(),buffer.names);
    std::copy(display.begin(),display.end(),buffer.names+substitute.size()+1);
    buffer.length=WORD(8+(substitute.size()+display.size()+2)*2);
    DWORD returned=0;const auto ok=DeviceIoControl(file,FSCTL_SET_REPARSE_POINT,&buffer,buffer.length+8,nullptr,0,&returned,nullptr);
    CloseHandle(file);if(!ok)RemoveDirectoryW(link.c_str());return bool(ok);
}
#endif
int run()
{
    const auto base=std::filesystem::absolute("native-save-store-test-fixture");
    require(!std::filesystem::exists(base),"fixture is exclusively owned");
    require(std::filesystem::create_directory(base),"fixture parent created");
    struct Cleanup {std::filesystem::path base;~Cleanup(){std::error_code error;std::filesystem::remove_all(base,error);}} cleanup{base};
    const auto root=base/"SAVES",outside=base/"OUTSIDE";
    std::filesystem::create_directory(outside);
    {std::ofstream file(outside/"KEEP");file<<"original";}
    NativeSaveStore store(root.generic_string());
    require(store.createFile("early")==E::Unavailable,"operations require initialized storage");
    require(store.initialize()==E::Success && store.initialize()==E::Success,"actual root created idempotently");
    auto space=store.freeBytes();require(bool(space) && space.value>0,"actual filesystem available bytes queried");
    require(store.stat("").value.directory,"root metadata is a real directory");
    require(store.mkdir("BASCUS-97472nico")==E::Success,"native application directory created");
    require(store.mkdir("BASCUS-97472nico")==E::Exists,"duplicate directory reports exists");
    constexpr auto path="BASCUS-97472nico/save.dat";
    require(store.createFile(path,8)==E::Success,"exclusive native file created at requested size");
    auto meta=store.stat(path);
    require(bool(meta) && meta.value.size==8 && !meta.value.directory && meta.value.name=="save.dat" &&
            meta.value.modifiedUnixSeconds>0 && meta.value.readable && meta.value.writable,"native file size/name/time/permissions preserved");
#ifdef _WIN32
    require(meta.value.createdUnixSeconds && *meta.value.createdUnixSeconds>0,"Windows native creation timestamp retained");
#else
    require(!meta.value.createdUnixSeconds,"POSIX metadata never invents creation time from inode change time");
#endif
    const std::array<uint8_t,4> data{1,2,3,4};
    auto write=store.writeAt(path,2,data);require(bool(write) && write.value==4,"positioned native write flushed and closed");
    require(store.createFile(path,0)==E::Exists,"exclusive duplicate never truncates user data");
    std::array<uint8_t,12> bytes{};
    auto read=store.readAt(path,0,bytes);
    require(bool(read) && read.value==8 && bytes[0]==0 && bytes[1]==0 && bytes[2]==1 && bytes[5]==4 && bytes[6]==0,
            "native read returns actual EOF count and preserves resized zero bytes");
    require(store.readAt(path,8,bytes).value==0,"EOF read returns zero");
    require(store.writeAt(path,UINT64_MAX,data).error==E::Generic,"oversized write offset rejects before mutation");
    require(store.readAt(path,UINT64_MAX,bytes).error==E::Generic,"oversized read offset rejects");
    require(store.createFile("oversized",UINT64_MAX)==E::Generic && store.stat("oversized").error==E::NotFound,
            "oversized exclusive creation cannot create a partial file");
    auto emptyWrite=store.writeAt(path,0,{});require(bool(emptyWrite) && emptyWrite.value==0,"empty positioned write succeeds on existing real file");
    auto listing=store.listDirectory("BASCUS-97472nico");
    require(bool(listing) && listing.value.size()==1 && listing.value[0].name=="save.dat" && listing.value[0].size==8,
            "actual directory enumeration exposes file metadata");
    auto rootListing=store.listDirectory("");
    require(bool(rootListing) && rootListing.value.size()==1 && rootListing.value[0].directory,"root enumeration preserves real subdirectory type");
    auto repeatedRootListing=store.listDirectory("");
    require(bool(repeatedRootListing) && repeatedRootListing.value.size()==rootListing.value.size() &&
            repeatedRootListing.value[0].name==rootListing.value[0].name &&
            repeatedRootListing.value[0].size==rootListing.value[0].size &&
            repeatedRootListing.value[0].directory==rootListing.value[0].directory &&
            repeatedRootListing.value[0].modifiedUnixSeconds==rootListing.value[0].modifiedUnixSeconds,
            "successive root enumerations use independent native directory positions");
    require(store.removeFile("BASCUS-97472nico")==E::Generic,"file deletion never recursively removes a directory");
    require(store.stat("missing").error==E::NotFound && store.readAt("missing",0,bytes).error==E::NotFound &&
            store.writeAt("missing",0,data).error==E::NotFound && store.removeFile("missing")==E::NotFound,
            "missing paths never become success or implicitly created writes");
    for(const auto invalid:{"../OUTSIDE/KEEP","BASCUS-97472nico/../../OUTSIDE/KEEP","/OUTSIDE/KEEP","C:/OUTSIDE/KEEP",
            "BASCUS-97472nico\\save.dat","BASCUS-97472nico//save.dat","./file","file/","file:stream","bad. ","bad.","NUL","CON.txt"}) {
        require(store.createFile(invalid)==E::Generic && store.writeAt(invalid,0,data).error==E::Generic &&
                store.removeFile(invalid)==E::Generic,"unsafe source path rejects before filesystem mutation");
    }
    const std::string embedded("abc\0def",7);
    require(store.createFile(embedded)==E::Generic && store.stat(embedded).error==E::Generic,"embedded null path rejects");
    require(store.createFile(std::string(NativeSaveStore::NameLimit+1,'x'))==E::Generic,"oversized name rejects");
    require(store.listDirectory(path).error==E::Generic && store.readAt("BASCUS-97472nico",0,bytes).error==E::Generic &&
            store.writeAt("BASCUS-97472nico",0,data).error==E::Generic,"file and directory operations preserve actual entry type");
    std::error_code hardlinkError;
    std::filesystem::create_hard_link(outside/"KEEP",root/"ALIAS",hardlinkError);
    if(!hardlinkError) {
        require(store.stat("ALIAS").error==E::Generic && store.writeAt("ALIAS",0,data).error==E::Generic &&
                store.removeFile("ALIAS")==E::Generic,"multiply linked files cannot mutate outside data through a local alias");
        std::filesystem::remove(root/"ALIAS");
    } else std::printf("SKIP: hard links unavailable (%s)\n",hardlinkError.message().c_str());
    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(outside,root/"LINK",symlinkError);
    bool linked=!symlinkError;
#ifdef _WIN32
    if(!linked)linked=junction(root/"LINK",outside);
    struct JunctionCleanup {std::filesystem::path path;~JunctionCleanup(){RemoveDirectoryW(path.c_str());}} linkCleanup{root/"LINK"};
#endif
    if(linked) {
        require(store.stat("LINK/KEEP").error==E::Generic && store.writeAt("LINK/KEEP",0,data).error==E::Generic &&
                store.createFile("LINK/new")==E::Generic && store.mkdir("LINK/dir")==E::Generic,
                "symlink parents cannot escape save root");
        require(store.removeFile("LINK")==E::Generic,"symlink leaf cannot be removed as an ordinary save file");
        require(!store.listDirectory(""),"unsafe symlink entries fail enumeration explicitly");
        NativeSaveStore linkedRoot((root/"LINK").generic_string());
        require(linkedRoot.initialize()==E::Generic,"save root itself cannot be a reparse or symlink entry");
        std::filesystem::remove(root/"LINK");
    } else std::printf("SKIP: symlink creation unavailable (%s)\n",symlinkError.message().c_str());
    {std::ifstream file(outside/"KEEP");std::string contents;file>>contents;require(contents=="original","outside file never modified");}
    require(!std::filesystem::exists(outside/"new") && !std::filesystem::exists(outside/"dir"),"no external entries created");
    require(store.removeFile(path)==E::Success && store.stat(path).error==E::NotFound,"explicit native file deletion verified");
    require(store.listDirectory("BASCUS-97472nico").value.empty(),"deleted file disappears from real enumeration");
    require(store.mkdir("MANY")==E::Success,"bounded enumeration fixture directory created");
    for(size_t i=0;i<=NativeSaveStore::DirectoryLimit;++i){std::ofstream file(root/"MANY"/std::to_string(i));file<<'x';}
    const auto tooMany=store.listDirectory("MANY");
    require(tooMany.error==E::Generic && tooMany.value.empty(),"directory cap reports failure without silently truncating results");
    NativeSaveStore absent((base/"absent"/"child").generic_string());
    require(absent.initialize()==E::NotFound,"missing trusted root parent is not recursively created");
    NativeSaveStore fileRoot((outside/"KEEP").generic_string());
    require(fileRoot.initialize()==E::Generic,"ordinary file cannot masquerade as storage root");
    std::printf("native save store: %u checks passed\n",checks);return 0;
}
}
int main(){try{return run();}catch(const std::exception &error){std::fprintf(stderr,"native save test: %s\n",error.what());return 1;}}
