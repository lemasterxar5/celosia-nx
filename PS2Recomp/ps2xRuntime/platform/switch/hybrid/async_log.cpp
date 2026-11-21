// v169 (CPU rewrite #1): stderr through a memory device ("log:"). Before, stderr was the SD file itself,
// unbuffered: every fprintf from any thread wrote the card before returning (the render worker's periodic
// statistics: 20-50 ms record spikes at frames 1800, 3600, 7500, 9000 ... in the cinematic). Now a write
// is a copy into a buffer; a low-priority thread writes it to the same file (hybrid-error.log) about every
// 100 ms. The fatal paths (terminate, native faults) write what is pending first (try-locks only: never
// wait there). sdmc:/switch/celosia_nx/sync-log present: the old synchronous file.
#include <switch.h>
#include <sys/iosupport.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {
FsFile logFile{};
bool logFileOk = false;
s64 logOffset = 0;
std::mutex pendingLock, fileLock;
std::string pending;
uint64_t droppedBytes = 0;
Thread writerThread;
UEvent wake;
constexpr size_t kMaxPending = 8u << 20;

void writeOut(const std::string &chunk)
{
    if (!logFileOk || chunk.empty())
        return;
    if (R_SUCCEEDED(fsFileWrite(&logFile, logOffset, chunk.data(), chunk.size(), FsWriteOption_None)))
        logOffset += static_cast<s64>(chunk.size());
}
void writerMain(void *)
{
    std::string chunk;
    for (;;)
    {
        (void)waitSingle(waiterForUEvent(&wake), 100000000ll);
        {
            std::lock_guard<std::mutex> guard(pendingLock);
            chunk.swap(pending);
            if (droppedBytes)
            {
                char note[96];
                const int n = std::snprintf(note, sizeof(note), "[log] %llu bytes dropped (buffer full)\n", (unsigned long long)droppedBytes);
                if (n > 0) chunk.append(note, static_cast<size_t>(n));
                droppedBytes = 0;
            }
        }
        if (!chunk.empty())
        {
            std::lock_guard<std::mutex> guard(fileLock);
            writeOut(chunk);
            (void)fsFileFlush(&logFile);
        }
        chunk.clear();
    }
}
ssize_t logWrite(struct _reent *, void *, const char *ptr, size_t len)
{
    {
        std::lock_guard<std::mutex> guard(pendingLock);
        if (pending.size() + len > kMaxPending)
            droppedBytes += len;
        else
            pending.append(ptr, len);
    }
    ueventSignal(&wake);
    return static_cast<ssize_t>(len);
}
int logOpen(struct _reent *, void *, const char *, int, int) { return 0; }
int logClose(struct _reent *, void *) { return 0; }
devoptab_t logDevice;
}

// Opens hybrid-error.log for the writer and points stderr at the memory device. False: nothing changed.
bool startSwitchAsyncLog()
{
    if (FILE *marker = std::fopen("sdmc:/switch/celosia_nx/sync-log", "rb"))
    {
        std::fclose(marker);
        return false;
    }
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        return false;
    constexpr const char *path = "/switch/celosia_nx/hybrid-error.log";
    (void)fsFsDeleteFile(fs, path);
    if (R_FAILED(fsFsCreateFile(fs, path, 0, 0)))
        return false;
    if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &logFile)))
        return false;
    logFileOk = true;
    std::memset(&logDevice, 0, sizeof(logDevice));
    logDevice.name = "log";
    logDevice.structSize = sizeof(int);
    logDevice.open_r = logOpen;
    logDevice.close_r = logClose;
    logDevice.write_r = logWrite;
    if (AddDevice(&logDevice) < 0)
    {
        fsFileClose(&logFile);
        logFileOk = false;
        return false;
    }
    ueventCreate(&wake, true);
    if (R_FAILED(threadCreate(&writerThread, writerMain, nullptr, nullptr, 64u << 10, 0x3B, -2)) || R_FAILED(threadStart(&writerThread)))
    {
        RemoveDevice("log:");
        fsFileClose(&logFile);
        logFileOk = false;
        return false;
    }
    return std::freopen("log:/hybrid-error.log", "w", stderr) != nullptr;
}

// The fatal paths: what is pending written now, if no other thread holds the locks.
extern "C" void switchAsyncLogFlushNow()
{
    if (!logFileOk)
        return;
    std::string chunk;
    if (!pendingLock.try_lock())
        return;
    chunk.swap(pending);
    pendingLock.unlock();
    if (!fileLock.try_lock())
        return;
    writeOut(chunk);
    (void)fsFileFlush(&logFile);
    fileLock.unlock();
}
