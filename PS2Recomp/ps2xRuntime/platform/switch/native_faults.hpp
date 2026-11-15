#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace celosia::native {
struct NativeFaultInfo {
    uint64_t pc{}, lr{}, sp{}, far{};
    uint32_t esr{}, description{};
};

// Allocation/lock-free formatter. Returns bytes written (excluding terminator).
// Nonempty output is always NUL-terminated, including when truncated.
std::size_t formatNativeFault(const NativeFaultInfo &fault, std::span<char> output) noexcept;

class NativeFaults final {
public:
    // libnx installs the strong linked hook before main; this records availability.
    // Does not claim that a fault has been triggered or recovered.
    bool initialize() noexcept;
    bool initialized() const noexcept { return ready; }
private:
    bool ready=false;
};
}
