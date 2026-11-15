#include "native_heap.hpp"
#include "native_memory.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace celosia::native;
static unsigned checks = 0;
static void require(bool ok, const char *why) { ++checks; if (!ok) throw std::runtime_error(why); }
int main()
{
    DataMemory memory;
    DataHeap heap(memory);
    require(heap.allocate(16) == 0 && heap.base() == 0 && heap.limit() == 0, "unconfigured heap unusable");
    require(!heap.configure(0,4096) && !heap.configure(16,DataMemory::MainBytes+1), "invalid heap region");
    require(!heap.configure(UINT32_MAX,DataMemory::MainBytes) && !heap.configure(4096,4096), "configure overflow/order");
    require(heap.configure(4097,8192) && heap.base() == 4112 && heap.limit() == 8192, "checked aligned base");
    const auto a = heap.allocate(31), b = heap.allocate(64,64);
    require(a == 4112 && b == 4160, "aligned nonoverlap first-fit");
    require(!heap.configure(16384,32768), "live allocation blocks reconfigure");
    require(heap.allocate(UINT32_MAX) == 0 && heap.allocate(1,3) == 0 && heap.allocate(1,8192) == 0,
            "size/alignment overflow rejected");
    require(heap.allocate(0) == 0 && heap.calloc(UINT32_MAX,2) == 0 && heap.calloc(1,0) == 0, "zero and multiplication overflow");
    std::array<uint8_t,31> pattern; pattern.fill(0xAB);
    require(memory.write(a,pattern.data(),pattern.size()), "write allocation");
    const auto moved = heap.reallocate(a,100);
    require(moved != 0 && moved != a, "blocked growth moves");
    std::array<uint8_t,100> out{};
    require(memory.read(moved,out.data(),out.size()) && std::memcmp(out.data(),pattern.data(),31) == 0,
            "realloc preserves old bytes");
    for (size_t i=31;i<out.size();++i) require(out[i] == 0, "grown bytes zeroed");
    require(!heap.release(a) && !heap.release(moved+1), "old/interior pointer rejected");
    require(heap.reallocate(moved,UINT32_MAX) == 0 && memory.read(moved,out.data(),31) && out[0] == 0xAB,
            "failed realloc preserves original block");
    require(heap.reallocate(moved,16) == moved, "in-place shrink");
    require(heap.reallocate(moved,64) == moved, "in-place expansion into free tail");
    require(heap.release(moved|0x80000000u) && !heap.release(moved), "cached alias free and double-free rejection");
    require(heap.release(b|0xA0000000u), "uncached alias free");
    require(!heap.release(0x12000000) && heap.release(0), "device rejection/null free");
    require(heap.configure(0x1000,0x1100), "reconfigure without live blocks");
    std::array<uint8_t,256> dirty; dirty.fill(0xCD);
    require(memory.write(0x1000,dirty.data(),dirty.size()), "dirty future heap region");
    const auto c = heap.calloc(8,8), d = heap.allocate(64), e = heap.allocate(64), f = heap.allocate(64);
    std::array<uint8_t,64> cleared;
    require(memory.read(c,cleared.data(),cleared.size()) && cleared == std::array<uint8_t,64>{}, "calloc clears previous data");
    require(c == 0x1000 && d == 0x1040 && e == 0x1080 && f == 0x10C0 && heap.allocate(1) == 0, "exhaustion exact boundary");
    require(heap.release(d) && heap.release(e) && heap.allocate(128) == d, "adjacent gaps coalesce by first-fit");
    require(heap.reallocate(c,0) == 0 && !heap.release(c), "zero realloc frees");
    require(heap.reallocate(0,0) == 0 && heap.reallocate(0,16) == c, "null realloc allocates");
    require(heap.reallocate(0x12000000,16) == 0 && heap.reallocate(f+1,16) == 0, "invalid realloc rejected");
    DataHeap parallel(memory);
    require(parallel.configure(0x20000,0x40000), "concurrent test heap");
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (unsigned t=0;t<4;++t) workers.emplace_back([&,t] {
        for (unsigned i=0;i<500;++i) {
            const auto p = parallel.allocate(32,32);
            if (!p) { failed = true; continue; }
            std::array<uint8_t,32> bytes{}; bytes.fill(static_cast<uint8_t>(t+1));
            if (!memory.write(p,bytes.data(),bytes.size())) failed = true;
            std::array<uint8_t,32> read{};
            if (!memory.read(p,read.data(),read.size()) || read != bytes || !parallel.release(p)) failed = true;
        }
    });
    for (auto &worker : workers) worker.join();
    require(!failed, "concurrent allocation ownership and copies");
    require(parallel.allocate(0x20000) == 0x20000, "concurrent frees leave one reusable region");
    std::printf("native heap: %u checks passed\n", checks);
}
