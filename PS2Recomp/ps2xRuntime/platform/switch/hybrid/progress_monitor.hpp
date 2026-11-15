#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
namespace celosia::native {
// Diagnostic worker has its own small stack and joins before callback state dies.
class ProgressMonitor final {
    pthread_t thread{};
    std::function<void()> callback;
    const unsigned intervalMs;
    std::atomic<bool> stopping{false},failure{false};
    static void *entry(void *argument)noexcept
    {
        auto &self=*static_cast<ProgressMonitor *>(argument);
        auto next=std::chrono::steady_clock::now();
        while(!self.stopping.load(std::memory_order_acquire)){
            if(std::chrono::steady_clock::now()>=next){
                try{self.callback();}catch(...){self.failure.store(true,std::memory_order_release);break;}
                next=std::chrono::steady_clock::now()+std::chrono::milliseconds(self.intervalMs);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return nullptr;
    }
public:
    explicit ProgressMonitor(std::function<void()> function,unsigned periodMs=1000)
        :callback(std::move(function)),intervalMs(periodMs)
    {
        if(!callback || !intervalMs)throw std::invalid_argument("Invalid progress monitor");
        pthread_attr_t attributes;
        auto result=pthread_attr_init(&attributes);
        if(result)throw std::system_error(result,std::generic_category(),"Monitor attributes");
        result=pthread_attr_setstacksize(&attributes,128u*1024u);
        if(!result)result=pthread_create(&thread,&attributes,entry,this);
        pthread_attr_destroy(&attributes);
        if(result)throw std::system_error(result,std::generic_category(),"Monitor creation");
    }
    ~ProgressMonitor(){
        stopping.store(true,std::memory_order_release);
        if(pthread_join(thread,nullptr))std::terminate();
    }
    ProgressMonitor(const ProgressMonitor &)=delete;
    ProgressMonitor &operator=(const ProgressMonitor &)=delete;
    bool failed()const noexcept{return failure.load(std::memory_order_acquire);}
};
}
