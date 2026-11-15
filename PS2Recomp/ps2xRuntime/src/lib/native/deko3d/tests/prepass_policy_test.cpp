#include "../prepass_policy.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace native_renderer;
using namespace native_renderer::detail::deko;
namespace {
struct Item {DrawState s;uint8_t va=128,ta=128;};
PrepassPlan plan(const std::vector<Item> &items,std::vector<uint8_t> &flags){
    return planPrepass(items.size(),[&](std::size_t i)->const DrawState&{return items[i].s;},
        [&](std::size_t i,uint8_t &va,uint8_t &ta){va=items[i].va;ta=items[i].ta;},flags);
}
Item world(){Item i;i.s.texture=1;i.s.blend=Blend::Normal;i.s.opacity=1;return i;}
Item backdrop(){Item i;i.s.background=true;i.s.blend=Blend::Normal;i.s.opacity=1;return i;}
}
int main(){
    unsigned checks=0;
    auto check=[&](bool ok){++checks;if(!ok){std::fprintf(stderr,"prepass check %u failed\n",checks);throw std::runtime_error("Prepass policy regression");}};
    std::vector<uint8_t> flags;
    // Opaque textured world: 128/128 * 2*128/255 > 1.
    check(prepassEligible(world().s,128,128));
    check(!prepassEligible(world().s,127,128));       // shader alpha below 1 blends
    check(!prepassEligible(world().s,128,127));
    {auto w=world();w.s.blend=Blend::Opaque;check(prepassEligible(w.s,128,40));check(!prepassEligible(w.s,40,10));} // discard edge
    {auto w=world();w.s.texture=0;check(prepassEligible(w.s,128,0));check(!prepassEligible(w.s,127,0));}
    {auto w=world();w.s.opacity=0.99f;check(!prepassEligible(w.s,128,128));}
    {auto w=world();w.s.noDepthWrite=true;check(!prepassEligible(w.s,128,128));}
    {auto w=world();w.s.overlay=true;check(!prepassEligible(w.s,128,128));}
    {auto w=world();w.s.blend=Blend::Additive;check(!prepassEligible(w.s,128,128));}
    {auto w=world();w.s.fresnel=true;check(!prepassEligible(w.s,128,128));}
    {auto w=world();w.s.debugColour=0xFF0000;check(!prepassEligible(w.s,128,128));}
    check(!prepassEligible(backdrop().s,128,128));
    // Sky first, then world: the backdrop tests, the world is pre-passed.
    {
        std::vector<Item> v{backdrop(),backdrop(),world(),world()};
        auto p=plan(v,flags);
        check(p.active && p.first==2 && p.barrier==4);
        check(p.farBackdrop(0,v[0].s) && p.farBackdrop(1,v[1].s));
        check(!flags[0] && !flags[1] && flags[2] && flags[3]);
    }
    // A backdrop after the first depth write keeps painter order (it overwrites the world).
    {
        std::vector<Item> v{backdrop(),world(),backdrop(),world()};
        auto p=plan(v,flags);
        check(p.farBackdrop(0,v[0].s) && !p.farBackdrop(2,v[2].s));
    }
    // A blended depth writer stops the plan: nothing after it is pre-passed.
    {
        auto blended=world();blended.va=64;blended.s.writeDepth=true;
        check(writesDepth(blended.s,false) && !prepassEligible(blended.s,64,128));
        std::vector<Item> v{backdrop(),world(),blended,world()};
        auto p=plan(v,flags);
        check(p.active && p.barrier==2 && flags[1] && !flags[3]);
    }
    // Blended writer before any opaque world: no plan, backdrops untouched.
    {
        auto blended=world();blended.va=64;
        std::vector<Item> v{backdrop(),blended,world()};
        auto p=plan(v,flags);
        check(!p.active && !p.farBackdrop(0,v[0].s));
    }
    // Non-writing translucent draws do not stop the plan.
    {
        auto glow=world();glow.s.blend=Blend::Additive;
        std::vector<Item> v{backdrop(),glow,world(),glow,world()};
        auto p=plan(v,flags);
        check(p.active && p.barrier==5 && flags[2] && flags[4] && !flags[1]);
    }
    // Shadow volumes and framebuffer readers are barriers.
    {
        Item sv;sv.s.shadowVolume=true;
        std::vector<Item> v{backdrop(),world(),sv,world()};
        auto p=plan(v,flags);check(p.barrier==2 && !flags[3]);
        Item copy=world();copy.s.screenCopy=true;
        std::vector<Item> c{backdrop(),copy,world()};
        p=plan(c,flags);check(!p.active);
    }
    // Explicit backdrop depth tests keep their own state.
    {
        auto b=backdrop();b.s.screenDepthTest=true;
        std::vector<Item> v{b,world()};
        auto p=plan(v,flags);check(p.active && !p.farBackdrop(0,v[0].s));
    }
    std::printf("deko prepass policy: %u checks passed\n",checks);
}
