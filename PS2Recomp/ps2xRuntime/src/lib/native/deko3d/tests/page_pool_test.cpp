#include "../page_pool.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
int main(){
    using native_renderer::detail::deko::PagePool;
    unsigned checks=0,created=0;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Page pool regression");};
    PagePool<unsigned,4> pool([&]{return std::make_shared<unsigned>(++created);},2);
    auto a=pool.acquire(1),b=pool.acquire(4097);
    check(a->offset==0 && b->offset==4096 && a->resource==b->resource);
    auto retained=a;a.reset();
    auto c=pool.acquire(4096);check(c->offset==12288 && c->resource==retained->resource);
    retained.reset();
    auto reuse=pool.acquire(4096);check(reuse->offset==0 && reuse->resource==c->resource);
    auto other=pool.acquire(16384);check(other->resource!=c->resource && created==2);
    bool rejected=false;try{pool.acquire(1);}catch(const std::length_error &){rejected=true;}check(rejected);
    rejected=false;try{pool.acquire(0);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    rejected=false;try{pool.acquire(16385);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    b.reset();c.reset();reuse.reset();other.reset();
    auto full=pool.acquire(16384);check(full->offset==0 && created==2);
    std::shared_ptr<PagePool<unsigned,4>::Lease> surviving;
    {PagePool<unsigned,4> temporary([]{return std::make_shared<unsigned>(99);},1);surviving=temporary.acquire(4096);}
    check(*surviving->resource==99);surviving.reset();
    bool failed=false;
    try{PagePool<unsigned,4> failing([]()->std::shared_ptr<unsigned>{throw std::runtime_error("allocation");},1);failing.acquire(1);}
    catch(const std::runtime_error &){failed=true;}check(failed);
    std::printf("page pool: %u checks passed\n",checks);
}
