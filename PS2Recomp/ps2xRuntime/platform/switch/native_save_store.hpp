#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace celosia::native {
// Ordinary native files scoped to one application directory. No card image,
// device protocol, formatting or recursive deletion is exposed. The owner
// supplies a trusted root whose parent already exists. initialize creates only
// that root when absent. Operations serialize internally and retain no caller
// buffers or open file handles after returning.
class NativeSaveStore final {
public:
    enum class Error : int32_t {Success=0,Unavailable=1,FileBroken=4,StorageBroken=5,NotFound=6,Exists=7,Generic=8};
    template<class T> struct Result {
        Error error=Error::Success;
        T value{};
        explicit operator bool() const noexcept {return error==Error::Success;}
    };
    struct Metadata {
        std::string name;uint64_t size=0;int64_t modifiedUnixSeconds=0;bool directory=false;
        std::optional<int64_t> createdUnixSeconds;
        bool readable=false,writable=false;
    };
    static constexpr size_t PathLimit=512,NameLimit=127,DirectoryLimit=1024;
    explicit NativeSaveStore(std::string root);
    ~NativeSaveStore();
    NativeSaveStore(const NativeSaveStore &)=delete;
    NativeSaveStore &operator=(const NativeSaveStore &)=delete;
    Error initialize();
    Result<uint64_t> freeBytes();
    Result<Metadata> stat(std::string_view path);
    Error mkdir(std::string_view path);
    Error createFile(std::string_view path,uint64_t size=0);
    Error removeFile(std::string_view path);
    // Successful short reads represent EOF. Failed reads/writes retain the
    // actual transferred count; failed writes may already have modified bytes.
    Result<size_t> readAt(std::string_view path,uint64_t offset,std::span<uint8_t> destination);
    Result<size_t> writeAt(std::string_view path,uint64_t offset,std::span<const uint8_t> source);
    // Empty path denotes the root only for stat/listDirectory. A directory
    // exceeding DirectoryLimit fails rather than returning an incomplete list.
    Result<std::vector<Metadata>> listDirectory(std::string_view path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
