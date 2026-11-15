#pragma once
// Host test doubles only; production uses the SDK header.
#include <cstdint>
#include <chrono>
#include <thread>
inline bool appletMainLoop(){return true;}
inline uint64_t armGetSystemTick(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline uint64_t armTicksToNs(uint64_t ticks){return ticks;}
inline void svcSleepThread(int64_t ns){std::this_thread::sleep_for(std::chrono::nanoseconds(ns));}
