#include "../descriptor_slots.hpp"
#include <cstdio>
#include <stdexcept>
int main(){
    using native_renderer::detail::deko::DescriptorSlots;
    unsigned checks=0;auto check=[&](bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);};
    auto throws=[&](auto fn,const char*m){bool threw=false;try{fn();}catch(const std::exception&){threw=true;}check(threw,m);};
    throws([]{DescriptorSlots zero(0);},"zero capacity rejected");
    throws([]{DescriptorSlots huge(65537);},"oversized pool rejected");
    DescriptorSlots pool(3);
    check(pool.size()==3&&pool.inUse()==0,"empty pool");
    const auto a=pool.acquire(),b=pool.acquire(),c=pool.acquire();
    check(a==0&&b==1&&c==2&&pool.inUse()==3,"sequential slots");
    check(pool.acquire()==DescriptorSlots::None,"exhausted pool returns None");
    pool.release(b);check(pool.inUse()==2,"release frees one");
    throws([&]{pool.release(b);},"double release detected");
    throws([&]{pool.release(7);},"unknown slot rejected");
    check(pool.acquire()==1,"freed slot reused before growth");
    check(pool.acquire()==DescriptorSlots::None,"still bounded");
    pool.release(DescriptorSlots::None);check(pool.inUse()==3,"releasing None is a no-op");
    pool.release(a);pool.release(c);pool.release(1);check(pool.inUse()==0,"all released");
    check(pool.acquire()!=DescriptorSlots::None,"reusable after full release");
    std::printf("descriptor slots: %u checks passed\n",checks);
}
