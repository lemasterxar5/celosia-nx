#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "native_save_store.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif
namespace celosia::native {
namespace {
using E=NativeSaveStore::Error;
template<class T> using R=NativeSaveStore::Result<T>;
bool validPath(std::string_view path,bool rootAllowed)
{
    if(path.empty())return rootAllowed;
    if(path.size()>NativeSaveStore::PathLimit)return false;
    for(size_t begin=0;begin<path.size();) {
        const auto separator=path.find('/',begin);
        const auto part=path.substr(begin,separator==path.npos?path.size()-begin:separator-begin);
        if(part.empty() || part.size()>NativeSaveStore::NameLimit || part=="." || part==".." || part.back()=='.')return false;
        std::string stem;
        for(char c:part) {
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
            if(c>='a'&&c<='z')c=char(c-'a'+'A');
            stem.push_back(c);
        }
        stem.resize(stem.find('.')==stem.npos?stem.size():stem.find('.'));
        if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||
           (stem.size()==4&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9'))return false;
        if(separator==path.npos)return true;
        begin=separator+1;if(begin==path.size())return false;
    }
    return false;
}
E lastError()
{
#ifdef _WIN32
    switch(GetLastError()) {
    case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return E::NotFound;
    case ERROR_ALREADY_EXISTS:case ERROR_FILE_EXISTS:return E::Exists;
    case ERROR_DISK_FULL:case ERROR_HANDLE_DISK_FULL:case ERROR_WRITE_FAULT:case ERROR_READ_FAULT:return E::StorageBroken;
    case ERROR_NOT_READY:case ERROR_DEVICE_NOT_CONNECTED:return E::Unavailable;
    default:return E::Generic;
    }
#else
    switch(errno) {
    case ENOENT:return E::NotFound;
    case EEXIST:return E::Exists;
    case ENOSPC:case EIO:return E::StorageBroken;
    case ENODEV:case ENXIO:return E::Unavailable;
    default:return E::Generic;
    }
#endif
}
std::string leafName(std::string_view path)
{const auto at=path.rfind('/');return std::string(at==path.npos?path:path.substr(at+1));}
#ifdef _WIN32
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;explicit Handle(HANDLE handle):value(handle){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    Handle&operator=(Handle&& other) noexcept {if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=std::exchange(other.value,INVALID_HANDLE_VALUE);return *this;}
    explicit operator bool()const{return value!=INVALID_HANDLE_VALUE;}
    E close(){const auto old=std::exchange(value,INVALID_HANDLE_VALUE);return old==INVALID_HANDLE_VALUE||CloseHandle(old)?E::Success:lastError();}
};
struct Search {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Search(){if(value!=INVALID_HANDLE_VALUE)FindClose(value);}
    E close(){const auto old=std::exchange(value,INVALID_HANDLE_VALUE);return old==INVALID_HANDLE_VALUE||FindClose(old)?E::Success:lastError();}
};
std::wstring finalPath(HANDLE file)
{
    std::wstring path(32768,L'\0');
    const auto count=GetFinalPathNameByHandleW(file,path.data(),DWORD(path.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!count||count>=path.size())return {};
    path.resize(count);while(path.size()>1&&path.back()==L'\\')path.pop_back();return path;
}
bool contains(std::wstring_view root,std::wstring_view path,bool equal=false)
{
    return ((path.size()>root.size()&&path[root.size()]==L'\\')||(equal&&path.size()==root.size()))&&
        CompareStringOrdinal(root.data(),int(root.size()),path.data(),int(root.size()),TRUE)==CSTR_EQUAL;
}
R<NativeSaveStore::Metadata> metadata(HANDLE file,std::string name)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info))return {lastError()};
    if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
       (!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&info.nNumberOfLinks!=1))return {E::Generic};
    const uint64_t ticks=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    NativeSaveStore::Metadata result{std::move(name),(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,
        ticks>=116444736000000000ull?int64_t((ticks-116444736000000000ull)/10000000):0,
        bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY),{},true,!(info.dwFileAttributes&FILE_ATTRIBUTE_READONLY)};
    const uint64_t created=(uint64_t(info.ftCreationTime.dwHighDateTime)<<32)|info.ftCreationTime.dwLowDateTime;
    if(created>=116444736000000000ull)result.createdUnixSeconds=int64_t((created-116444736000000000ull)/10000000);
    if(result.directory)result.size=0;
    return {E::Success,std::move(result)};
}
#else
struct Handle {
    int value=-1;
    Handle()=default;explicit Handle(int fd):value(fd){}
    ~Handle(){if(value>=0)::close(value);}
    Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,-1)){}
    Handle&operator=(Handle&& other) noexcept {if(value>=0)::close(value);value=std::exchange(other.value,-1);return *this;}
    explicit operator bool()const{return value>=0;}
    E close(){const auto old=std::exchange(value,-1);return old<0||::close(old)==0?E::Success:lastError();}
};
struct CloseDirectory {void operator()(DIR *directory)const noexcept {if(directory)closedir(directory);}};
R<NativeSaveStore::Metadata> metadataInfo(const struct stat &info,std::string name)
{
    if((!S_ISREG(info.st_mode)&&!S_ISDIR(info.st_mode))||
       (S_ISREG(info.st_mode)&&info.st_nlink!=1))return {E::Generic};
    if(info.st_size<0)return {E::FileBroken};
    NativeSaveStore::Metadata result{std::move(name),S_ISDIR(info.st_mode)?0:uint64_t(info.st_size),int64_t(info.st_mtime),
        bool(S_ISDIR(info.st_mode)),{},bool(info.st_mode&0444),bool(info.st_mode&0222)};
#ifdef __SWITCH__
    // libnx fsdev maps st_ctime to actual filesystem creation time, unlike
    // ordinary POSIX inode-change time. Directory timestamps may be unavailable.
    if(info.st_ctime>0)result.createdUnixSeconds=int64_t(info.st_ctime);
#endif
    return {E::Success,std::move(result)};
}
R<NativeSaveStore::Metadata> metadata(int file,std::string name)
{struct stat info{};if(fstat(file,&info)!=0)return {lastError()};return metadataInfo(info,std::move(name));}
#endif
}
struct NativeSaveStore::Impl {
    std::string root;
    std::mutex mutex;
    bool ready=false;
#ifdef _WIN32
    Handle anchor;
    std::wstring resolvedRoot;
#elif !defined(__SWITCH__)
    Handle anchor;
#endif
    struct Path {
        E error=E::Success;
        std::string full,leaf;
        std::vector<Handle> parents;
    };
    explicit Impl(std::string path):root(std::move(path))
    {
        if(root.empty()||root.size()>4096||root.find('\0')!=root.npos)throw std::invalid_argument("Native save root is invalid");
#ifdef _WIN32
        root=std::filesystem::absolute(root).lexically_normal().generic_string();
#endif
        while(root.size()>1&&root.back()=='/')root.pop_back();
    }
    Path path(std::string_view relative,bool rootAllowed=false)
    {
        Path result;
        if(!ready){result.error=E::Unavailable;return result;}
        if(!validPath(relative,rootAllowed)){result.error=E::Generic;return result;}
        result.full=root+(relative.empty()?"":"/"+std::string(relative));result.leaf=leafName(relative);
        std::string partial=root;
#if !defined(_WIN32) && !defined(__SWITCH__)
        result.parents.emplace_back(dup(anchor.value));
        if(!result.parents.back()){result.error=lastError();return result;}
#endif
        for(size_t begin=0;begin<relative.size();) {
            const auto at=relative.find('/',begin);if(at==relative.npos)break;
            const auto part=std::string(relative.substr(begin,at-begin));partial+="/"+part;
#ifdef _WIN32
            Handle parent(CreateFileW(std::filesystem::path(partial).c_str(),FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!parent){result.error=lastError();return result;}
            auto info=metadata(parent.value,part);
            if(!info||!info.value.directory||!contains(resolvedRoot,finalPath(parent.value))){result.error=E::Generic;return result;}
            result.parents.push_back(std::move(parent));
#elif defined(__SWITCH__)
            struct stat info{};
            if(lstat(partial.c_str(),&info)!=0){result.error=lastError();return result;}
            if(!S_ISDIR(info.st_mode)||S_ISLNK(info.st_mode)){result.error=E::Generic;return result;}
#else
            Handle parent(openat(result.parents.back().value,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW));
            if(!parent){result.error=lastError();return result;}
            result.parents.push_back(std::move(parent));
#endif
            begin=at+1;
        }
        return result;
    }
    R<Handle> open(Path &path,bool write=false,bool create=false)
    {
        if(path.error!=E::Success)return {path.error};
#ifdef _WIN32
        Handle file(CreateFileW(std::filesystem::path(path.full).c_str(),FILE_READ_ATTRIBUTES|GENERIC_READ|(write?GENERIC_WRITE:0),
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,create?CREATE_NEW:OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!file)return {lastError()};
        if(!contains(resolvedRoot,finalPath(file.value),true))return {E::Generic};
#elif defined(__SWITCH__)
        struct stat before{};
        if(lstat(path.full.c_str(),&before)==0) {if(S_ISLNK(before.st_mode))return {E::Generic};}
        else if(errno!=ENOENT||!create)return {lastError()};
        Handle file(::open(path.full.c_str(),(write?O_RDWR:O_RDONLY)|(create?(O_CREAT|O_EXCL):0),0600));
        if(!file)return {lastError()};
#else
        // dup() would share the persistent anchor's directory offset. A fresh
        // open file description keeps successive root enumerations independent.
        Handle file(path.leaf.empty()?openat(anchor.value,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW):openat(path.parents.back().value,path.leaf.c_str(),
            (write?O_RDWR:O_RDONLY)|O_NOFOLLOW|(create?(O_CREAT|O_EXCL):0),0600));
        if(!file)return {lastError()};
#endif
        const auto info=metadata(file.value,path.leaf);
        if(!info)return {info.error};
        if(write&&info.value.directory)return {E::Generic};
        return {E::Success,std::move(file)};
    }
};
NativeSaveStore::NativeSaveStore(std::string root):impl(std::make_unique<Impl>(std::move(root))){}
NativeSaveStore::~NativeSaveStore()=default;
NativeSaveStore::Error NativeSaveStore::initialize()
{
    std::lock_guard lock(impl->mutex);auto &s=*impl;if(s.ready)return E::Success;
#ifdef _WIN32
    const auto path=std::filesystem::path(s.root);
    if(!CreateDirectoryW(path.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return lastError();
    Handle root(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!root)return lastError();
    auto info=metadata(root.value,"");if(!info||!info.value.directory)return E::Generic;
    s.resolvedRoot=finalPath(root.value);if(s.resolvedRoot.empty())return E::Generic;s.anchor=std::move(root);
#else
    if(::mkdir(s.root.c_str(),0700)!=0&&errno!=EEXIST)return lastError();
    struct stat info{};if(lstat(s.root.c_str(),&info)!=0)return lastError();
    if(!S_ISDIR(info.st_mode)||S_ISLNK(info.st_mode))return E::Generic;
#ifndef __SWITCH__
    Handle root(::open(s.root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW));if(!root)return lastError();s.anchor=std::move(root);
#endif
#endif
    s.ready=true;return E::Success;
}
R<uint64_t> NativeSaveStore::freeBytes()
{
    std::lock_guard lock(impl->mutex);if(!impl->ready)return {E::Unavailable};
#ifdef _WIN32
    ULARGE_INTEGER available{};
    if(!GetDiskFreeSpaceExW(impl->resolvedRoot.c_str(),&available,nullptr,nullptr))return {lastError()};
    return {E::Success,available.QuadPart};
#else
    struct statvfs info{};
    if(statvfs(impl->root.c_str(),&info)!=0)return {lastError()};
    const uint64_t unit=info.f_frsize?info.f_frsize:info.f_bsize;
    if(unit&&uint64_t(info.f_bavail)>UINT64_MAX/unit)return {E::StorageBroken};
    return {E::Success,uint64_t(info.f_bavail)*unit};
#endif
}
R<NativeSaveStore::Metadata> NativeSaveStore::stat(std::string_view path)
{
    std::lock_guard lock(impl->mutex);auto checked=impl->path(path,true);
#ifdef __SWITCH__
    if(checked.error!=E::Success)return {checked.error};
    struct stat info{};
    if(lstat(checked.full.c_str(),&info)!=0)return {lastError()};
    return metadataInfo(info,checked.leaf);
#else
    auto file=impl->open(checked);
    if(!file)return {file.error};
    auto result=metadata(file.value.value,checked.leaf);
    const auto closed=file.value.close();if(closed!=E::Success)result.error=closed;return result;
#endif
}
NativeSaveStore::Error NativeSaveStore::mkdir(std::string_view path)
{
    std::lock_guard lock(impl->mutex);auto checked=impl->path(path);if(checked.error!=E::Success)return checked.error;
#ifdef _WIN32
    if(!CreateDirectoryW(std::filesystem::path(checked.full).c_str(),nullptr))return lastError();
#elif defined(__SWITCH__)
    if(::mkdir(checked.full.c_str(),0700)!=0)return lastError();
#else
    if(mkdirat(checked.parents.back().value,checked.leaf.c_str(),0700)!=0)return lastError();
#endif
#ifdef __SWITCH__
    struct stat info{};if(lstat(checked.full.c_str(),&info)!=0)return lastError();
    return S_ISDIR(info.st_mode)&&!S_ISLNK(info.st_mode)?E::Success:E::Generic;
#else
    auto opened=impl->open(checked);if(!opened)return opened.error;return opened.value.close();
#endif
}
NativeSaveStore::Error NativeSaveStore::createFile(std::string_view path,uint64_t size)
{
    std::lock_guard lock(impl->mutex);if(size>INT64_MAX)return E::Generic;
    auto checked=impl->path(path);auto file=impl->open(checked,true,true);if(!file)return file.error;
    E error=E::Success;
#ifdef _WIN32
    LARGE_INTEGER position{};position.QuadPart=size;
    if(!SetFilePointerEx(file.value.value,position,nullptr,FILE_BEGIN)||!SetEndOfFile(file.value.value))error=lastError();
    if(!FlushFileBuffers(file.value.value)&&error==E::Success)error=lastError();
#else
    static_assert(sizeof(off_t)>=8);
    if(ftruncate(file.value.value,static_cast<off_t>(size))!=0)error=lastError();
    if(fsync(file.value.value)!=0&&error==E::Success)error=lastError();
#endif
    const auto closed=file.value.close();return error!=E::Success?error:closed;
}
NativeSaveStore::Error NativeSaveStore::removeFile(std::string_view path)
{
    std::lock_guard lock(impl->mutex);auto checked=impl->path(path);if(checked.error!=E::Success)return checked.error;
#ifdef _WIN32
    Handle file(CreateFileW(std::filesystem::path(checked.full).c_str(),DELETE|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(!file)return lastError();auto info=metadata(file.value,checked.leaf);
    if(!info)return info.error;
    if(info.value.directory||!contains(impl->resolvedRoot,finalPath(file.value)))return E::Generic;
    FILE_DISPOSITION_INFO remove{TRUE};if(!SetFileInformationByHandle(file.value,FileDispositionInfo,&remove,sizeof(remove)))return lastError();
    return file.close();
#else
    auto file=impl->open(checked);if(!file)return file.error;auto info=metadata(file.value.value,checked.leaf);
    if(!info)return info.error;
    if(info.value.directory)return E::Generic;
#ifdef __SWITCH__
    if(unlink(checked.full.c_str())!=0)return lastError();
#else
    if(unlinkat(checked.parents.back().value,checked.leaf.c_str(),0)!=0)return lastError();
#endif
    return file.value.close();
#endif
}
namespace {
template<class Buffer> R<size_t> transfer(Handle &file,uint64_t offset,Buffer bytes,bool write)
{
    R<size_t> result;
#ifdef _WIN32
    LARGE_INTEGER position{};position.QuadPart=offset;
    if(!SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN))return {lastError()};
#else
    if(lseek(file.value,static_cast<off_t>(offset),SEEK_SET)<0)return {lastError()};
#endif
    while(result.value<bytes.size()) {
        const size_t count=std::min<size_t>(bytes.size()-result.value,1024*1024);
#ifdef _WIN32
        DWORD moved=0;
        const auto ok=write?WriteFile(file.value,bytes.data()+result.value,DWORD(count),&moved,nullptr):
            ReadFile(file.value,const_cast<uint8_t*>(bytes.data()+result.value),DWORD(count),&moved,nullptr);
        if(!ok){result.error=lastError();break;}
#else
        const auto moved=write?::write(file.value,bytes.data()+result.value,count):
            ::read(file.value,const_cast<uint8_t*>(bytes.data()+result.value),count);
        if(moved<0){if(errno==EINTR)continue;result.error=lastError();break;}
#endif
        result.value+=size_t(moved);
        if(moved==0){if(write)result.error=E::StorageBroken;break;}
    }
    if(write) {
#ifdef _WIN32
        if(!FlushFileBuffers(file.value)&&result.error==E::Success)result.error=lastError();
#else
        if(fsync(file.value)!=0&&result.error==E::Success)result.error=lastError();
#endif
    }
    const auto closed=file.close();if(closed!=E::Success&&result.error==E::Success)result.error=closed;
    return result;
}
}
R<size_t> NativeSaveStore::readAt(std::string_view path,uint64_t offset,std::span<uint8_t> bytes)
{
    std::lock_guard lock(impl->mutex);
    if(offset>INT64_MAX||bytes.size()>uint64_t(INT64_MAX)-offset||(!bytes.data()&&!bytes.empty()))return {E::Generic};
    auto checked=impl->path(path);auto file=impl->open(checked);if(!file)return {file.error};
    auto info=metadata(file.value.value,checked.leaf);if(!info)return {info.error};
    if(info.value.directory)return {E::Generic};
    return transfer(file.value,offset,bytes,false);
}
R<size_t> NativeSaveStore::writeAt(std::string_view path,uint64_t offset,std::span<const uint8_t> bytes)
{
    std::lock_guard lock(impl->mutex);
    if(offset>INT64_MAX||bytes.size()>uint64_t(INT64_MAX)-offset||(!bytes.data()&&!bytes.empty()))return {E::Generic};
    auto checked=impl->path(path);auto file=impl->open(checked,true);if(!file)return {file.error};
    return transfer(file.value,offset,bytes,true);
}
R<std::vector<NativeSaveStore::Metadata>> NativeSaveStore::listDirectory(std::string_view path)
{
    std::lock_guard lock(impl->mutex);auto checked=impl->path(path,true);
#ifdef __SWITCH__
    if(checked.error!=E::Success)return {checked.error};
    struct stat directoryInfo{};
    if(lstat(checked.full.c_str(),&directoryInfo)!=0)return {lastError()};
    if(!S_ISDIR(directoryInfo.st_mode)||S_ISLNK(directoryInfo.st_mode))return {E::Generic};
#else
    auto directory=impl->open(checked);
    if(!directory)return {directory.error};
    auto info=metadata(directory.value.value,checked.leaf);
    if(!info)return {info.error};
    if(!info.value.directory)return {E::Generic};
#endif
    R<std::vector<Metadata>> result;
    auto append=[&](const char *name) {
        const std::string_view part(name);if(part=="."||part=="..")return true;
        if(!validPath(part,false)||part.find('/')!=part.npos||result.value.size()>=DirectoryLimit){result.error=E::Generic;return false;}
        const auto relative=std::string(path)+(path.empty()?"":"/")+std::string(part);
        auto entryPath=impl->path(relative);
#ifdef __SWITCH__
        if(entryPath.error!=E::Success){result.error=entryPath.error;return false;}
        struct stat entryInfo{};if(lstat(entryPath.full.c_str(),&entryInfo)!=0){result.error=lastError();return false;}
        auto details=metadataInfo(entryInfo,std::string(part));if(!details){result.error=details.error;return false;}
#else
        auto file=impl->open(entryPath);if(!file){result.error=file.error;return false;}
        auto details=metadata(file.value.value,std::string(part));if(!details){result.error=details.error;return false;}
        const auto closed=file.value.close();if(closed!=E::Success){result.error=closed;return false;}
#endif
        result.value.push_back(std::move(details.value));return true;
    };
#ifdef _WIN32
    WIN32_FIND_DATAW entry{};
    Search search{FindFirstFileW(std::filesystem::path(checked.full+"/*").c_str(),&entry)};
    if(search.value==INVALID_HANDLE_VALUE) {if(GetLastError()!=ERROR_FILE_NOT_FOUND)result.error=lastError();}
    else {
        do {
            std::string name;
            for(const auto c:std::wstring_view(entry.cFileName)) {if(c>127){result.error=E::Generic;break;}name.push_back(char(c));}
            if(result.error!=E::Success||!append(name.c_str()))break;
        }while(FindNextFileW(search.value,&entry));
        if(result.error==E::Success&&GetLastError()!=ERROR_NO_MORE_FILES)result.error=lastError();
        const auto closed=search.close();if(closed!=E::Success&&result.error==E::Success)result.error=closed;
    }
#else
#ifdef __SWITCH__
    DIR *entries=opendir(checked.full.c_str());
#else
    const int duplicate=dup(directory.value.value);DIR *entries=duplicate<0?nullptr:fdopendir(duplicate);
    if(!entries&&duplicate>=0)::close(duplicate);
#endif
    std::unique_ptr<DIR,CloseDirectory> entriesLease(entries);
    if(!entries)result.error=lastError();
    else {
        for(;;) {errno=0;const auto *entry=readdir(entries);if(!entry){if(errno)result.error=lastError();break;}if(!append(entry->d_name))break;}
        if(closedir(entriesLease.release())!=0&&result.error==E::Success)result.error=lastError();
    }
#endif
#ifndef __SWITCH__
    const auto closed=directory.value.close();if(closed!=E::Success&&result.error==E::Success)result.error=closed;
#endif
    if(!result)result.value.clear();
    return result;
}
}
