#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace celosia::native {
// Read-only native asset files, confined to one configured root. No drive,
// sector, ISO or device behavior is modeled. Tokens are not OS descriptors.
class NativeFiles final {
public:
    struct Metadata {
        uint64_t size=0;
        // Unix seconds from the opened native handle. Missing creation time
        // stays absent: POSIX ctime is a metadata-change time, not birth time.
        std::optional<int64_t> created,accessed,modified;
    };
    static constexpr unsigned MaxLive=256;
    static constexpr size_t PathLimit=1024;
    explicit NativeFiles(std::string root);
    ~NativeFiles();
    NativeFiles(const NativeFiles &)=delete;
    NativeFiles &operator=(const NativeFiles &)=delete;
    // Invalid paths throw. ASCII components become uppercase; terminal ;1 is
    // removed. open() also accepts only full paths returned by this root.
    std::string translate(std::string_view gamePath) const;
    uint64_t open(std::string_view gamePathOrTranslated);
    std::optional<uint64_t> size(uint64_t token) const;
    // Regular files only. Reuses the same confined opening/handle checks as
    // read(), without allocating a token or changing any existing cursor.
    std::optional<Metadata> stat(std::string_view path) const;
    // May return a short read at EOF. An I/O error can leave partial output.
    std::optional<size_t> read(uint64_t token,uint64_t offset,std::span<uint8_t> output);
    bool close(uint64_t token);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
