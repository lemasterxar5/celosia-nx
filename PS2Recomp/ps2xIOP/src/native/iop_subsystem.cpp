#include "ps2x/iop/iop_subsystem.h"
#include "ps2x/iop/ps2_path.h"
#include "audio_arena.h"
#include "iop_service.h"
#include "iop_module_manager.h"
#include "module_factories.h"
#include "emulator/services/iop_sg2.h" // Native SG2 driver, now independent of IopMemory/CPU.
#include <algorithm>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>

namespace ps2x::iop {
namespace {
constexpr std::array<std::string_view,2> SoundAliases{"sg2iopm1", "sg2iop_driver"};
}
class IopSubsystem::Impl {
public:
    explicit Impl(IopHost &h) : host(h), sound(h, arena)
    {
        services.push_back(detail::createMcservService(host));
        services.push_back(detail::createDbcmanService(host));
        services.push_back(detail::createLibSdService(host));
        services.push_back(detail::createNativeControllerService(host));
        std::vector<std::string> keys;
        for (const auto alias : SoundAliases) keys.emplace_back(alias);
        for (const auto &service : services)
            for (const auto alias : service->moduleAliases()) keys.emplace_back(alias);
        modules.setServiceModuleKeys(std::move(keys));
        sound.reset();
    }
    void rebuildRoutes()
    {
        routes.clear();
        for (const auto &service : services) {
            if (!modules.isLoaded(service->moduleAliases())) continue;
            for (const auto sid : service->sids()) {
                if (!routes.emplace(sid, service.get()).second)
                    throw std::runtime_error("native services: duplicate RPC SID");
            }
        }
    }
    void error(std::string message)
    {
        if (message != lastError) host.log(LogLevel::Error, message);
        lastError = std::move(message);
    }
    IopHost &host;
    detail::AudioArena arena;
    detail::Sg2Driver sound;
    detail::IopModuleManager modules;
    detail::ServiceList services;
    std::unordered_map<uint32_t, detail::IopService *> routes;
    // Audio and RPC share this lock; only service reset/load takes it exclusively.
    // SoundMixer retains its short per-chunk lock handoff for the game thread.
    mutable std::shared_mutex epoch;
    mutable std::mutex metadata;
    bool soundLoaded = false;
    std::string lastError;
};
IopSubsystem::IopSubsystem(IopHost &host) : m_impl(std::make_unique<Impl>(host)) {}
IopSubsystem::~IopSubsystem() = default;
IopSubsystem::IopSubsystem(IopSubsystem &&) noexcept = default;
IopSubsystem &IopSubsystem::operator=(IopSubsystem &&) noexcept = default;
void IopSubsystem::reset()
{
    std::unique_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    m_impl->soundLoaded = false;
    m_impl->sound.reset();
    m_impl->arena.reset();
    m_impl->modules.reset();
    m_impl->routes.clear();
    m_impl->lastError.clear();
    for (const auto &service : m_impl->services) service->reset();
}
ModuleLoadResult IopSubsystem::loadModule(std::string_view path, const void *, uint32_t)
{
    std::unique_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    if (path.empty() || path.size() > 1024 || path.find('\0') != std::string_view::npos ||
        !parsePs2Path(path)) return {true, -1, -1};
    const auto key=ps2PathLeafKey(path);
    bool provided=std::find(SoundAliases.begin(),SoundAliases.end(),key)!=SoundAliases.end();
    for(const auto &service:m_impl->services) {
        const auto aliases=service->moduleAliases();
        if(std::find(aliases.begin(),aliases.end(),key)==aliases.end()) continue;
        provided=true;
        if(!service->activate()) {
            m_impl->error("native services: native provider initialization failed for "+std::string(path));
            return {true,-1,-1};
        }
    }
    if(!provided) {
        m_impl->error("native services: no native provider for "+std::string(path));
        return {true,-1,-1};
    }
    const auto result = m_impl->modules.loadHle(path);
    if (result.moduleId > 0) {
        if (std::find(SoundAliases.begin(), SoundAliases.end(), key) != SoundAliases.end()) {
            if (!m_impl->soundLoaded) m_impl->sound.reset();
            m_impl->soundLoaded = true;
        }
        m_impl->rebuildRoutes();
    } else m_impl->error("native services: no provider for module " + std::string(path));
    return result;
}
ModuleLoadResult IopSubsystem::loadModuleBuffer(uint32_t, const void *, uint32_t)
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    m_impl->error("native services: executable module buffers are not supported");
    return {true, -1, -1};
}
bool IopSubsystem::stopModule(int32_t id, int32_t *result)
{
    std::unique_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    if (!m_impl->modules.stopHle(id, result)) return false;
    m_impl->soundLoaded = m_impl->modules.isLoaded(SoundAliases);
    for(const auto &service:m_impl->services)
        if(!m_impl->modules.isLoaded(service->moduleAliases())) service->reset();
    m_impl->rebuildRoutes();
    return true;
}
void IopSubsystem::runEeCycles(uint64_t) noexcept
{
    // No IOP clock or instruction scheduler exists in this build. The native
    // audio device requests frames directly from the host mixer in readAudio.
}
size_t IopSubsystem::readAudio(int16_t *destination, size_t frames) noexcept
{
    // Bound a single device pull; the platform streams shorter chunks.
    constexpr size_t MaxPullFrames = 4096;
    if (!destination || !frames || frames > MaxPullFrames) return 0;
    std::shared_lock epoch(m_impl->epoch);
    return m_impl->soundLoaded ? m_impl->sound.mixer().readOutput(destination, frames) : 0;
}
bool IopSubsystem::soundReady() const
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    return m_impl->soundLoaded;
}
bool IopSubsystem::submitSoundCommands(std::span<const std::array<uint32_t,4>> commands,
                                       std::span<uint8_t,400> reply)
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    return m_impl->soundLoaded && m_impl->sound.submitCommandBatch(commands,reply);
}
RpcAbi IopSubsystem::selectRpcAbi(const RpcAbiRequest &request) const
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    for (const auto &service : m_impl->services) {
        if (!m_impl->modules.isLoaded(service->moduleAliases())) continue;
        const auto abi = service->selectRpcAbi(request);
        if (abi != RpcAbi::RuntimeDefault) return abi;
    }
    return RpcAbi::RuntimeDefault;
}
bool IopSubsystem::canBindRpc(uint32_t sid) const noexcept
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    return (m_impl->soundLoaded && sid == detail::Sg2Driver::Sid) || m_impl->routes.count(sid);
}
RpcResult IopSubsystem::handleRpc(const RpcRequest &request)
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    if (m_impl->soundLoaded && request.sid == detail::Sg2Driver::Sid) {
        // The driver validates the complete command batch and reply region.
        return m_impl->sound.handleRpc(request);
    }
    const auto found = m_impl->routes.find(request.sid);
    if (found != m_impl->routes.end()) return found->second->handleRpc(request);
    m_impl->error("native services: unsupported RPC SID " + std::to_string(request.sid));
    return {}; // No guest server, interpreter or synthetic success fallback.
}
bool IopSubsystem::enterNativeMode() { return true; }
void IopSubsystem::onSifTransfer(const SifTransfer &transfer)
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    for (const auto &service : m_impl->services)
        if (m_impl->modules.isLoaded(service->moduleAliases())) service->onSifTransfer(transfer);
}
uint32_t IopSubsystem::allocateMemory(uint32_t size, uint32_t alignment) { return m_impl->arena.allocate(size, alignment); }
bool IopSubsystem::freeMemory(uint32_t address) { return m_impl->arena.freeAllocation(address); }
bool IopSubsystem::readMemory(uint32_t a, void *out, size_t n) const { return m_impl->arena.readRam(a, out, n); }
bool IopSubsystem::writeMemory(uint32_t a, const void *in, size_t n) { return m_impl->arena.writeRam(a, in, n); }
bool IopSubsystem::zeroMemory(uint32_t a, size_t n) { return m_impl->arena.zeroRam(a, n); }
bool IopSubsystem::isMemoryRange(uint32_t a, size_t n) const { return m_impl->arena.ownsRamRange(a, n); }
DebugSnapshot IopSubsystem::debugSnapshot() const
{
    std::shared_lock epoch(m_impl->epoch);
    std::lock_guard metadata(m_impl->metadata);
    DebugSnapshot result;
    if (!m_impl->lastError.empty()) result.diagnostics.push_back(m_impl->lastError);
    for (const auto &service : m_impl->services) {
        DebugService row;
        row.name = service->name();
        row.sids.assign(service->sids().begin(), service->sids().end());
        row.active = m_impl->modules.isLoaded(service->moduleAliases());
        service->appendDebugMetrics(row.metrics);
        result.services.push_back(std::move(row));
    }
    DebugService sound;
    sound.name = "sg2-native"; sound.sids = {detail::Sg2Driver::Sid}; sound.active = m_impl->soundLoaded;
    result.services.push_back(std::move(sound));
    return result; // All emulator metrics remain zero: those objects do not exist.
}
}
