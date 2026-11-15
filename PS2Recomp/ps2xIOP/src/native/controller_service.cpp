#include "module_factories.h"
#include <array>
#include <cstdio>

namespace ps2x::iop::detail {
namespace {
class NativeControllerService final : public IopService {
    IopHost &host;
    bool active=false;
    static constexpr std::array<std::string_view,3> Aliases{"sio2man","sio2d","ds1o_d"};
public:
    explicit NativeControllerService(IopHost &owner) : host(owner) {}
    ~NativeControllerService() override
    {
        try { reset(); } catch(...) { std::fputs("Native controller cleanup failed\n",stderr); }
    }
    std::string_view name() const override { return "native-controller"; }
    std::span<const uint32_t> sids() const override { return {}; }
    std::span<const std::string_view> moduleAliases() const override { return Aliases; }
    bool activate() override
    {
        if(active) return true;
        try { active=host.initializeNativeController(); }
        catch(...) { active=false; }
        return active;
    }
    void reset() override
    {
        if(!active) return;
        active=false; host.resetNativeController();
    }
    RpcResult handleRpc(const RpcRequest &) override { return {}; }
};
}
std::unique_ptr<IopService> createNativeControllerService(IopHost &host)
{ return std::make_unique<NativeControllerService>(host); }
}
