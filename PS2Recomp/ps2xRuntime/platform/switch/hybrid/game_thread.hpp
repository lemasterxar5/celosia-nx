#pragma once
#include <pthread.h>
#include <exception>
#include <functional>
#include <system_error>
#include <utility>
#include "thread_placement.hpp"
namespace celosia::native {
// The recompiled game can use the scheduler's 384-KiB host-stack budget.
// Configure its POSIX worker explicitly without changing other threads' defaults.
class GameThread final {
    pthread_t thread{};
    std::function<void()> routine;
    bool live=false;
    static void *entry(void *argument)noexcept
    {
        placeSwitchThread("game",0);
        try{static_cast<GameThread *>(argument)->routine();}
        catch(...){std::terminate();}
        return nullptr;
    }
public:
    explicit GameThread(std::function<void()> function):routine(std::move(function))
    {
        if(!routine)throw std::invalid_argument("Game thread has no entry");
        pthread_attr_t attributes;
        auto result=pthread_attr_init(&attributes);
        if(result)throw std::system_error(result,std::generic_category(),"Game thread attributes");
        result=pthread_attr_setstacksize(&attributes,2u*1024u*1024u);
        if(!result)result=pthread_create(&thread,&attributes,entry,this);
        pthread_attr_destroy(&attributes);
        if(result)throw std::system_error(result,std::generic_category(),"Game thread creation");
        live=true;
    }
    ~GameThread(){if(live)std::terminate();}
    GameThread(const GameThread &)=delete;
    GameThread &operator=(const GameThread &)=delete;
    bool joinable()const noexcept{return live;}
    void join()
    {
        if(!live)throw std::logic_error("Game thread already joined");
        const auto result=pthread_join(thread,nullptr);
        if(result)throw std::system_error(result,std::generic_category(),"Game thread join");
        live=false;routine={};
    }
};
}
