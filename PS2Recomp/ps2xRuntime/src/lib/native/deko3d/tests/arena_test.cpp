#include "../arena.hpp"
#include <cstdio>
#include <stdexcept>
using native_renderer::detail::deko::ArenaCursor;
int main(){
    unsigned checks=0;
    auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("Arena regression");};
    ArenaCursor cursor;
    auto a=cursor.reserve(100);check(a.page==0 && a.offset==0 && a.capacity>=100);
    auto b=cursor.reserve(17);check(b.page==0 && b.offset==112);
    auto c=cursor.reserve(1024*1024);check(c.page==1 && c.offset==0);
    cursor.reset();auto d=cursor.reserve(100);check(d.page==a.page && d.offset==a.offset && d.capacity==a.capacity);
    d=cursor.reserve(1024*1024);check(d.page==c.page && d.offset==0);
    bool rejected=false;try{cursor.reserve(0);}catch(const std::invalid_argument&){rejected=true;}check(rejected);
    rejected=false;try{cursor.reserve(128u*1024u*1024u+1);}catch(const std::invalid_argument&){rejected=true;}check(rejected);
    ArenaCursor full;full.reserve(128u*1024u*1024u);
    rejected=false;try{full.reserve(1);}catch(const std::length_error&){rejected=true;}check(rejected);
    full.reset();check(full.reserve(16).offset==0);
    // Vertices and uint32 indices share fenced pages; ranges must not overlap,
    // including when index data spills onto another page and after reuse.
    ArenaCursor mixed;
    const auto vertices=mixed.reserve(3u*48u),indices=mixed.reserve(3u*sizeof(uint32_t));
    check(vertices.page==indices.page && indices.offset>=vertices.offset+3u*48u && indices.offset%16u==0);
    const auto large=mixed.reserve(1024u*1024u);
    check(large.page!=indices.page && large.offset==0);
    mixed.reset();const auto again=mixed.reserve(3u*48u),indexAgain=mixed.reserve(3u*sizeof(uint32_t));
    check(again.page==vertices.page && indexAgain.offset==indices.offset);
    std::printf("arena: %u checks passed\n",checks);
}
