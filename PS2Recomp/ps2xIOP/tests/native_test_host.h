#pragma once
#include "ps2x/iop/iop_host.h"
#include <vector>
#include <cstring>
#include <stdexcept>

class NativeTestHost final : public ps2x::iop::IopHost {
public:
    std::vector<uint8_t> guest = std::vector<uint8_t>(65536, 0xAA);
    std::vector<std::string> messages;
    uint32_t failReadAt = UINT32_MAX;
    uint32_t failWriteAt = UINT32_MAX;
    bool readGuest(uint32_t a, void *out, size_t n) const override {
        if (n && a == failReadAt) return false;
        if (a > guest.size() || n > guest.size() - a || (!out && n)) return false;
        if (n) std::memcpy(out, guest.data() + a, n);
        return true;
    }
    bool writeGuest(uint32_t a, const void *in, size_t n) override {
        if (n && a == failWriteAt) return false;
        if (a > guest.size() || n > guest.size() - a || (!in && n)) return false;
        if (n) std::memcpy(guest.data() + a, in, n);
        return true;
    }
    bool zeroGuest(uint32_t a, size_t n) override {
        if (a > guest.size() || n > guest.size() - a) return false;
        std::memset(guest.data() + a, 0, n); return true;
    }
    bool normalizeGuestAddress(uint32_t a, uint32_t &out) const override {
        out = a; return a < guest.size();
    }
    uint32_t allocateIopHandle(ps2x::iop::IopHandleKind) override { return ++handle; }
    uint32_t allocateGuest(uint32_t, uint32_t) override { return 0; }
    void freeGuest(uint32_t) override {}
    void audioCommand(uint32_t, uint32_t, ps2x::iop::GuestBuffer, ps2x::iop::GuestBuffer) override {}
    std::string hostPath(ps2x::iop::HostPathKind) const override { return {}; }
    std::string translateGuestPath(std::string_view) const override { return {}; }
    uint64_t openHostFile(std::string_view) override { return 0; }
    bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
    bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &read) override { read = 0; return false; }
    void closeHostFile(uint64_t) override {}
    int32_t memoryCard(const ps2x::iop::MemoryCardRequest &) override { return -1; }
    bool hasGuestFunction(uint32_t) const override { return false; }
    bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override {
        throw std::logic_error("native service attempted guest callback");
    }
    void log(ps2x::iop::LogLevel, std::string_view text) override { messages.emplace_back(text); }
private:
    uint32_t handle = 0;
};
