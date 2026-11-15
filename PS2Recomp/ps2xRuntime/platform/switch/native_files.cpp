#include "native_files.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <sys/stat.h>
#ifndef __SWITCH__
#include <filesystem>
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif
namespace celosia::native {
namespace {
char upper(char c) { return c>='a' && c<='z' ? char(c-'a'+'A') : c; }
bool beginsCd(std::string_view value)
{
    constexpr std::string_view prefix="CDROM0:";
    if(value.size()<prefix.size()) return false;
    for(size_t i=0;i<prefix.size();++i) if(upper(value[i])!=prefix[i]) return false;
    return true;
}
std::string relativeName(std::string value)
{
    if(value.empty() || value.size()>NativeFiles::PathLimit) throw std::invalid_argument("Native asset path length");
    std::replace(value.begin(),value.end(),'\\','/');
    if(beginsCd(value)) { value.erase(0,7); if(value.starts_with('/')) value.erase(0,1); }
    if(value.ends_with(";1")) value.resize(value.size()-2);
    size_t start=0;
    while(start<value.size()) {
        const auto end=value.find('/',start);
        const auto component=std::string_view(value).substr(start,end==std::string::npos?value.size()-start:end-start);
        if(component.empty() || component=="." || component=="..") throw std::invalid_argument("Native asset traversal path");
        for(const auto c:component)
            if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.'))
                throw std::invalid_argument("Native asset path character");
        if(end==std::string::npos) break;
        start=end+1;
        if(start==value.size()) throw std::invalid_argument("Native asset trailing separator");
    }
    if(value.empty() || value.front()=='/') throw std::invalid_argument("Native asset absolute path");
    std::transform(value.begin(),value.end(),value.begin(),upper);
    return value;
}
#ifdef _WIN32
std::wstring finalPath(HANDLE handle)
{
    std::wstring result(32768,L'\0');
    const auto size=GetFinalPathNameByHandleW(handle,result.data(),DWORD(result.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size || size>=result.size()) throw std::runtime_error("Native asset resolved path unavailable");
    result.resize(size); return result;
}
bool contained(const std::wstring &root,const std::wstring &file)
{
    return file.size()>root.size() && file[root.size()]==L'\\' &&
        CompareStringOrdinal(root.data(),int(root.size()),file.data(),int(root.size()),TRUE)==CSTR_EQUAL;
}
#endif
struct CloseFile { void operator()(FILE *file) const noexcept { if(file) std::fclose(file); } };
using File=std::unique_ptr<FILE,CloseFile>;
}
struct NativeFiles::Impl {
    std::string root;
    mutable std::mutex mutex;
    std::unordered_map<uint64_t,File> files;
    uint64_t next=1;
#ifdef _WIN32
    std::wstring resolvedRoot;
#endif
    explicit Impl(std::string path) : root(std::move(path))
    {
        if(root.empty() || root.size()>PathLimit || root.find('\0')!=std::string::npos)
            throw std::invalid_argument("Native asset root");
#ifndef __SWITCH__
        root=std::filesystem::canonical(root).generic_string();
#else
        std::replace(root.begin(),root.end(),'\\','/');
#endif
        while(root.size()>1 && root.back()=='/') root.pop_back();
        struct stat info{};
        if(::stat(root.c_str(),&info)!=0 || !S_ISDIR(info.st_mode)) throw std::invalid_argument("Native asset root is not a directory");
#ifdef _WIN32
        const auto native=std::filesystem::path(root).wstring();
        const auto directory=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
                                        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(directory==INVALID_HANDLE_VALUE) throw std::runtime_error("Native asset root cannot open");
        try { resolvedRoot=finalPath(directory); } catch(...) { CloseHandle(directory); throw; }
        CloseHandle(directory);
        while(resolvedRoot.size()>1 && resolvedRoot.back()==L'\\') resolvedRoot.pop_back();
#endif
    }
    std::string translate(std::string_view path) const
    {
        std::string value(path);
        if(value.starts_with(root+"/")) value.erase(0,root.size()+1);
        return root+"/"+relativeName(std::move(value));
    }
    File openFile(const std::string &path) const
    {
        // Reject links/junctions for every asset component, then verify the
        // opened Windows handle as well to close path-replacement races.
        for(size_t end=root.size()+1;end<=path.size();++end) {
            if(end!=path.size() && path[end]!='/') continue;
            const auto component=path.substr(0,end);
#ifdef _WIN32
            const auto attributes=GetFileAttributesW(std::filesystem::path(component).wstring().c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return {};
#else
            struct stat part{};
            if(lstat(component.c_str(),&part)!=0 || S_ISLNK(part.st_mode)) return {};
#endif
        }
#ifdef _WIN32
        File file(_wfopen(std::filesystem::path(path).wstring().c_str(),L"rb"));
        if(file) {
            const auto handle=reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file.get())));
            if(!contained(resolvedRoot,finalPath(handle))) return {};
        }
#else
        File file(std::fopen(path.c_str(),"rb"));
#endif
        if(!file) return {};
#ifdef _WIN32
        struct _stat64 info{};
        if(_fstat64(_fileno(file.get()),&info)!=0 || (info.st_mode&_S_IFMT)!=_S_IFREG) return {};
#else
        struct stat info{};
        if(fstat(fileno(file.get()),&info)!=0 || !S_ISREG(info.st_mode)) return {};
#endif
        return file;
    }
};
NativeFiles::NativeFiles(std::string root) : impl(std::make_unique<Impl>(std::move(root))) {}
NativeFiles::~NativeFiles()=default;
std::string NativeFiles::translate(std::string_view path) const { return impl->translate(path); }
uint64_t NativeFiles::open(std::string_view path)
{
    std::lock_guard lock(impl->mutex);
    if(impl->files.size()>=MaxLive || impl->next==std::numeric_limits<uint64_t>::max()) return 0;
    try {
        auto file=impl->openFile(impl->translate(path));
        if(!file) return 0;
        const auto token=impl->next++;
        impl->files.emplace(token,std::move(file)); return token;
    } catch(const std::exception &) { return 0; }
}
std::optional<uint64_t> NativeFiles::size(uint64_t token) const
{
    std::lock_guard lock(impl->mutex);
    const auto found=impl->files.find(token);
    if(found==impl->files.end()) return {};
#ifdef _WIN32
    struct _stat64 info{};
    if(_fstat64(_fileno(found->second.get()),&info)!=0 || info.st_size<0) return {};
#else
    struct stat info{};
    if(fstat(fileno(found->second.get()),&info)!=0 || info.st_size<0) return {};
#endif
    return static_cast<uint64_t>(info.st_size);
}
std::optional<NativeFiles::Metadata> NativeFiles::stat(std::string_view path) const
{
    std::lock_guard lock(impl->mutex);
    try {
        const auto file=impl->openFile(impl->translate(path));
        if(!file) return {};
        Metadata result;
#ifdef _WIN32
        struct _stat64 info{};
        if(_fstat64(_fileno(file.get()),&info)!=0 || info.st_size<0) return {};
        // The Windows CRT maps st_ctime to the real creation time.
        result.created=static_cast<int64_t>(info.st_ctime);
#else
        struct stat info{};
        if(fstat(fileno(file.get()),&info)!=0 || info.st_size<0) return {};
#endif
        result.size=static_cast<uint64_t>(info.st_size);
        result.accessed=static_cast<int64_t>(info.st_atime);
        result.modified=static_cast<int64_t>(info.st_mtime);
        return result;
    } catch(const std::exception &) { return {}; }
}
std::optional<size_t> NativeFiles::read(uint64_t token,uint64_t offset,std::span<uint8_t> output)
{
    std::lock_guard lock(impl->mutex);
    const auto found=impl->files.find(token);
    if(found==impl->files.end() || offset>INT64_MAX || (!output.data() && !output.empty())) return {};
    if(output.empty()) return 0;
    auto *file=found->second.get();
    std::clearerr(file);
#ifdef _WIN32
    if(_fseeki64(file,static_cast<int64_t>(offset),SEEK_SET)!=0) return {};
#else
    static_assert(sizeof(off_t)>=8);
    if(fseeko(file,static_cast<off_t>(offset),SEEK_SET)!=0) return {};
#endif
    const auto count=std::fread(output.data(),1,output.size(),file);
    if(std::ferror(file)) return {};
    return count;
}
bool NativeFiles::close(uint64_t token)
{ std::lock_guard lock(impl->mutex); return impl->files.erase(token)!=0; }
}
