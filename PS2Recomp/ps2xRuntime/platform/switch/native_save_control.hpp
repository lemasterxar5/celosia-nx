#pragma once
#include "native_save_store.hpp"
#include <cstdint>
#include <optional>
#include <string>
struct R5900Context;
namespace celosia::native {
class DataMemory;
// Game save operations backed by actual application files. Profile0 is the
// configured native storage; no second device, MC2 socket or card image exists.
// Calls run synchronously in the game's existing save worker/execution gate.
class NativeSaveControl final {
public:
    NativeSaveControl(DataMemory &data,NativeSaveStore &store):data(data),store(store) {}
    bool handle(uint32_t address,R5900Context &context);
private:
    using Error=NativeSaveStore::Error;
    static constexpr uint32_t MaxPayload=8u*1024u*1024u;
    DataMemory &data;
    NativeSaveStore &store;
    std::string directory;
    bool initialized=false;
    std::optional<std::string> path(uint32_t address,bool wildcard=false) const;
    Error operate(uint32_t address,const R5900Context &context);
    Error info(uint32_t output);
    Error readFile(const std::string &name,uint32_t destination,uint32_t offset,uint32_t count);
    Error writeFile(const std::string &name,uint32_t source,uint32_t offset,uint32_t count);
    Error list(const std::string &pattern,const R5900Context &context);
};
}
