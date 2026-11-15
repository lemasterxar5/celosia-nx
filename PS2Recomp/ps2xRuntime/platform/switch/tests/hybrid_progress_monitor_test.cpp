#include "progress_monitor.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
int main()
{
    unsigned checks=0;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Progress monitor regression");};
    std::atomic<unsigned> calls{0};
    const auto parent=std::this_thread::get_id();std::atomic<bool> separate{false};
    {
        celosia::native::ProgressMonitor monitor([&]{separate=std::this_thread::get_id()!=parent;++calls;},10);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(calls<2 && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        check(calls>=2 && separate);
    }
    const auto stopped=calls.load();std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(calls==stopped);
    bool rejected=false;
    try{celosia::native::ProgressMonitor empty({},10);}catch(const std::invalid_argument &){rejected=true;}
    check(rejected);
    {
        celosia::native::ProgressMonitor failed([]{throw std::runtime_error("probe");},10);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!failed.failed() && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        check(failed.failed());
    }
    std::printf("progress monitor: %u checks passed\n",checks);
}
