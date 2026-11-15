// Compiled only into CELOSIA_PGO=GENERATE builds (instrumented, never a release).
//
// The game is closed from HOME, which terminates the process without running atexit, so the
// gcov runtime would never write its profiles. A low-priority thread dumps them every
// kDumpSeconds and resets the counters; libgcov merges each dump into the existing .gcda files,
// so the files accumulate the whole session.
//
// A Windows-hosted GCC joins the profile directory and the mangled object name with '\', which
// is neither a separator nor a valid FAT character on the console. Calls to fopen are wrapped
// (link option --wrap=fopen in this build only) and only paths under the profile directory are
// rewritten; every other fopen is passed through unchanged.
#include <switch.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#ifndef CELOSIA_PGO_DIR
#error "CELOSIA_PGO_DIR must be defined for the PGO dump helper"
#endif

extern "C" {
void __gcov_dump(void);
void __gcov_reset(void);
FILE *__real_fopen(const char *path, const char *mode);

FILE *__wrap_fopen(const char *path, const char *mode) {
    static const char prefix[] = CELOSIA_PGO_DIR;
    const size_t length = sizeof(prefix) - 1;
    if (path && std::strncmp(path, prefix, length) == 0) {
        char fixed[512];
        const size_t total = std::strlen(path);
        if (total < sizeof(fixed)) {
            std::memcpy(fixed, path, total + 1);
            for (size_t i = length; i < total; ++i)
                if (fixed[i] == '\\') fixed[i] = '/';
            return __real_fopen(fixed, mode);
        }
    }
    return __real_fopen(path, mode);
}
}

namespace {
constexpr u64 kDumpSeconds = 30;
Thread dumpThread;
// libgcov merges each .gcda through stdio buffers; keep generous headroom.
alignas(0x1000) unsigned char dumpStack[0x80000];

// Append-only trace (opened per line so a frozen process still leaves complete lines).
void trace(const char *what, unsigned n) {
    if (FILE *f = __real_fopen(CELOSIA_PGO_DIR "/dump-trace.txt", "a")) {
        std::fprintf(f, "%s %u t=%llu ms\n", what, n,
            static_cast<unsigned long long>(armTicksToNs(armGetSystemTick()) / 1000000ULL));
        std::fclose(f);
    }
}

void dumpLoop(void *) {
    trace("thread", 0);
    for (unsigned n = 1;; ++n) {
        svcSleepThread(kDumpSeconds * 1000000000ULL);
        trace("dump-begin", n);
        __gcov_dump();
        __gcov_reset();
        trace("dump-end", n);
    }
}

struct PgoDumpStarter {
    PgoDumpStarter() {
        mkdir(CELOSIA_PGO_DIR, 0777);
        // libgcov reports open/merge failures on stderr, which otherwise goes nowhere.
        std::freopen(CELOSIA_PGO_DIR "/gcov-stderr.txt", "w", stderr);
        trace("start", 0);
        // High priority (0x20): the dump holds the stdio/FS locks the game also takes; at the lowest
        // priority (0x3B) it was starved mid-file and froze the game (training build only, so the
        // brief hitch every kDumpSeconds is acceptable). 0x3F is outside the application range.
        const Result rc = threadCreate(&dumpThread, dumpLoop, nullptr, dumpStack, sizeof(dumpStack), 0x20, -2);
        trace(R_SUCCEEDED(rc) ? "created" : "create-failed", rc);
        if (R_SUCCEEDED(rc)) threadStart(&dumpThread);
    }
} starter;
}
