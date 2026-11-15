#include "game_thread.hpp"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
int main()
{
    unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Game worker regression");};
    const auto parent=std::this_thread::get_id();std::atomic<bool> ran=false,separate=false;
    celosia::native::GameThread worker([&]{
        volatile unsigned char stack[512*1024];
        for(unsigned index=0;index<sizeof(stack);index+=4096)stack[index]=9;
        stack[sizeof(stack)-1]=23;
        separate=std::this_thread::get_id()!=parent;ran=stack[0]==9&&stack[sizeof(stack)-1]==23;
    });
    check(worker.joinable());worker.join();check(!worker.joinable()&&ran&&separate);
    bool rejected=false;try{worker.join();}catch(const std::logic_error &){rejected=true;}check(rejected);
    rejected=false;try{celosia::native::GameThread empty(std::function<void()>{});}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    std::printf("hybrid game worker: %u checks passed\n",checks);
}
