// Save states: the EE kernel's part (threads, their waits, the ready queues, semaphores, event
// flags, alarms, interrupt handlers, timed events, the VBlank count and the EE clock).
//
// Saved only at a safe point (safeForState): no guest code running, no interrupt / callback
// invocation open, nothing waiting on host work that cannot be redone (an asynchronous disc
// read, an MPEG frame), and every completion one that loadState can rebuild from its kind.
// Host times (the deadlines' host clock) are saved relative to now and rebased at load.
#include "runtime/ee_scheduler.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <chrono>

namespace
{
    constexpr uint32_t kStateVersion = 1;

    void writeWait(ps2state::StateWriter &out, const EeWaitState &w)
    {
        out.u8(static_cast<uint8_t>(w.reason));
        out.u8(static_cast<uint8_t>(w.payload.index()));
        std::visit(
            [&](const auto &p)
            {
                using T = std::decay_t<decltype(p)>;
                if constexpr (!std::is_same_v<T, std::monostate>)
                    out.pod(p);
            },
            w.payload);
        out.u8(w.resumeKind);
        out.u32(w.resumeArg);
    }

    bool readWait(ps2state::StateReader &in, EeWaitState &w)
    {
        w.reason = static_cast<EeWaitReason>(in.u8());
        switch (in.u8())
        {
        case 0: w.payload = std::monostate{}; break;
        case 1: w.payload = in.pod<EeSemaphoreWait>(); break;
        case 2: w.payload = in.pod<EeEventFlagWait>(); break;
        case 3: w.payload = in.pod<EeVSyncWait>(); break;
        case 4: w.payload = in.pod<EeExternalWait>(); break;
        default: in.fail("bad wait payload"); return false;
        }
        w.resumeKind = in.u8();
        w.resumeArg = in.u32();
        w.completion = EeScheduler::resumeFor(w.resumeKind, w.resumeArg);
        return in.ok();
    }

    void writeDeque(ps2state::StateWriter &out, const std::deque<int> &d)
    {
        out.u32(static_cast<uint32_t>(d.size()));
        for (int v : d)
            out.i32(v);
    }

    void readDeque(ps2state::StateReader &in, std::deque<int> &d)
    {
        d.clear();
        const uint32_t n = in.u32();
        if (n > 4096u)
        {
            in.fail("bad queue size");
            return;
        }
        for (uint32_t i = 0; i < n && in.ok(); ++i)
            d.push_back(in.i32());
    }

    template <typename Map, typename WriteValue>
    void writeMap(ps2state::StateWriter &out, const Map &m, WriteValue write)
    {
        // Sorted by key: the same state gives the same bytes.
        std::vector<typename Map::key_type> keys;
        keys.reserve(m.size());
        for (const auto &kv : m)
            keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        out.u32(static_cast<uint32_t>(keys.size()));
        for (const auto &k : keys)
        {
            out.pod(k);
            write(m.at(k));
        }
    }
}

std::function<void(R5900Context &)> EeScheduler::resumeFor(uint8_t kind, uint32_t arg)
{
    switch (kind)
    {
    case kResumeSetPc:
        return [arg](R5900Context &context) { context.pc = arg; };
    case kResumeReturn0:
        return [](R5900Context &resume)
        {
            R5900Context *r = &resume;
            SET_GPR_U64(r, 2, 0u);
            r->pc = GPR_U32(r, 31);
        };
    default:
        return {};
    }
}

bool EeScheduler::safeForState(std::string &why) const
{
    if (m_currentThreadId != 0)
    {
        why = "a thread is running";
        return false;
    }
    if (!m_pendingInvocations.empty())
    {
        why = "an invocation is pending";
        return false;
    }
    if (m_insideInterrupt)
    {
        why = "inside an interrupt";
        return false;
    }
    for (const auto &[id, t] : m_threads)
    {
        if (!t.invocations.empty())
        {
            why = "thread " + std::to_string(id) + " has an open invocation";
            return false;
        }
        if (t.resumeCompletion && t.resumeCompletionKind == kResumeNone)
        {
            why = "thread " + std::to_string(id) + " has a completion to run";
            return false;
        }
        if (t.wait.completion && t.wait.resumeKind == kResumeNone)
        {
            why = "thread " + std::to_string(id) + " waits with a host completion (disc read / stream / MPEG)";
            return false;
        }
        if (t.wait.reason == EeWaitReason::Mpeg)
        {
            why = "thread " + std::to_string(id) + " waits on MPEG";
            return false;
        }
        if (t.wait.reason == EeWaitReason::External)
        {
            const auto *external = std::get_if<EeExternalWait>(&t.wait.payload);
            if (!external || external->type != 0x44454C59u) // only DelayThread's timed waits
            {
                why = "thread " + std::to_string(id) + " waits on host work";
                return false;
            }
        }
    }
    return true;
}

void EeScheduler::saveState(ps2state::StateWriter &out) const
{
    out.tag("EEKS");
    out.u32(kStateVersion);
    out.u64(eeNow());
    out.u64(m_accountedCycle);
    out.u64(m_sliceEndCycle);
    out.u32(m_pendingEeTimerInterrupts);

    // Threads
    writeMap(out, m_threads,
             [&](const GuestThread &t)
             {
                 out.i32(t.id);
                 out.pod(t.context);
                 out.u32(t.entry);
                 out.u32(t.stack);
                 out.u32(t.stackSize);
                 out.u32(t.gp);
                 out.u32(t.attr);
                 out.u32(t.option);
                 out.u32(t.arg);
                 out.i32(t.initialPriority);
                 out.i32(t.currentPriority);
                 out.u8(static_cast<uint8_t>(t.status));
                 out.i32(t.suspendCount);
                 out.u32(t.wakeupCount);
                 out.flag(t.ownsStack);
                 out.u32(t.tlsBase);
                 writeWait(out, t.wait);
                 out.u8(t.resumeCompletion ? t.resumeCompletionKind : 0u);
                 out.u32(t.resumeCompletionArg);
             });
    for (const auto &q : m_readyQueues)
        writeDeque(out, q);

    writeMap(out, m_semaphores,
             [&](const EeSemaphore &s)
             {
                 out.i32(s.id);
                 out.i32(s.count);
                 out.i32(s.maxCount);
                 out.i32(s.initCount);
                 out.u32(s.attr);
                 out.u32(s.option);
                 writeDeque(out, s.waiters);
             });
    writeMap(out, m_eventFlags,
             [&](const EeEventFlag &f)
             {
                 out.i32(f.id);
                 out.u32(f.attr);
                 out.u32(f.option);
                 out.u32(f.initBits);
                 out.u32(f.bits);
                 writeDeque(out, f.waiters);
             });
    writeMap(out, m_alarms, [&](const EeAlarm &a) { out.pod(a); });
    writeMap(out, m_intcHandlers, [&](const EeIrqHandler &h) { out.pod(h); });
    writeMap(out, m_dmacHandlers, [&](const EeIrqHandler &h) { out.pod(h); });

    out.i32(m_nextThreadId);
    out.i32(m_nextInvocationThreadId);
    out.i32(m_nextSemaphoreId);
    out.i32(m_nextEventFlagId);
    out.i32(m_nextAlarmId);
    out.u64(m_nextDelayToken);
    out.i32(m_nextIntcHandlerId);
    out.i32(m_nextDmacHandlerId);
    out.i32(m_intcHeadOrder);
    out.i32(m_intcTailOrder);
    out.i32(m_dmacHeadOrder);
    out.i32(m_dmacTailOrder);
    out.u32(m_enabledIntcMask);
    out.u32(m_enabledDmacMask);
    out.flag(m_rescheduleRequested);
    out.flag(m_timeSliceExpired);

    // Timed events and queued events (host deadlines relative to now)
    {
        std::lock_guard lock(m_eventMutex);
        const auto hostNow = std::chrono::steady_clock::now();
        out.u32(static_cast<uint32_t>(m_deadlines.size()));
        for (const auto &d : m_deadlines)
        {
            out.u64(d.deadlineCycle);
            const int64_t inNs = std::chrono::duration_cast<std::chrono::nanoseconds>(d.hostDeadline - hostNow).count();
            out.pod(inNs);
            out.pod(d.event);
            out.u64(d.sequence);
        }
        out.u32(static_cast<uint32_t>(m_events.size()));
        for (const auto &e : m_events)
            out.pod(e);
        out.u64(m_eventSequence);
    }
    out.u64(m_invocationSequence);
    out.u64(m_vsyncTick);
    out.u32(m_vsyncFlagAddress);
    out.u32(m_vsyncTickAddress);
    out.u32(m_gsVSyncCallback);
    out.u32(m_gsVSyncCallbackGp);
    out.u32(m_gsVSyncCallbackSp);
    writeMap(out, m_invocationStackTops, [&](uint32_t v) { out.u32(v); });

    out.blob(m_vu0Global);
    out.flag(m_vu0OwnerValid);
    out.i32(m_vu0Owner.threadId);
    out.u64(m_vu0Owner.depth);
    out.u64(m_vu0Owner.sequence);
    out.tag("EEKE");
}

bool EeScheduler::loadState(ps2state::StateReader &in)
{
    assertExecutor();
    if (!in.tag("EEKS"))
        return false;
    if (in.u32() != kStateVersion)
    {
        in.fail("EE kernel state version");
        return false;
    }
    const uint64_t now = in.u64();
    const uint64_t accounted = in.u64();
    const uint64_t sliceEnd = in.u64();
    const uint32_t timerIrqs = in.u32();

    std::unordered_map<int, GuestThread> threads;
    {
        const uint32_t n = in.u32();
        if (n > 512u)
            in.fail("bad thread count");
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            const int key = in.pod<int>();
            GuestThread t;
            t.id = in.i32();
            t.context = in.pod<R5900Context>();
            t.entry = in.u32();
            t.stack = in.u32();
            t.stackSize = in.u32();
            t.gp = in.u32();
            t.attr = in.u32();
            t.option = in.u32();
            t.arg = in.u32();
            t.initialPriority = in.i32();
            t.currentPriority = in.i32();
            t.status = static_cast<EeThreadStatus>(in.u8());
            t.suspendCount = in.i32();
            t.wakeupCount = in.u32();
            t.ownsStack = in.flag();
            t.tlsBase = in.u32();
            readWait(in, t.wait);
            t.resumeCompletionKind = in.u8();
            t.resumeCompletionArg = in.u32();
            t.resumeCompletion = EeScheduler::resumeFor(t.resumeCompletionKind, t.resumeCompletionArg);
            if (t.status == EeThreadStatus::Running)
                t.status = EeThreadStatus::Ready; // nothing runs at a safe point
            threads.emplace(key, std::move(t));
        }
    }
    std::array<std::deque<int>, kPriorityCount> ready;
    for (auto &q : ready)
        readDeque(in, q);

    std::unordered_map<int, EeSemaphore> semaphores;
    {
        const uint32_t n = in.u32();
        if (n > 4096u)
            in.fail("bad semaphore count");
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            const int key = in.pod<int>();
            EeSemaphore s;
            s.id = in.i32();
            s.count = in.i32();
            s.maxCount = in.i32();
            s.initCount = in.i32();
            s.attr = in.u32();
            s.option = in.u32();
            readDeque(in, s.waiters);
            semaphores.emplace(key, std::move(s));
        }
    }
    std::unordered_map<int, EeEventFlag> flags;
    {
        const uint32_t n = in.u32();
        if (n > 4096u)
            in.fail("bad event flag count");
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            const int key = in.pod<int>();
            EeEventFlag f;
            f.id = in.i32();
            f.attr = in.u32();
            f.option = in.u32();
            f.initBits = in.u32();
            f.bits = in.u32();
            readDeque(in, f.waiters);
            flags.emplace(key, std::move(f));
        }
    }
    auto readPodMap = [&](auto &target, uint32_t limit)
    {
        using Value = typename std::decay_t<decltype(target)>::mapped_type;
        target.clear();
        const uint32_t n = in.u32();
        if (n > limit)
        {
            in.fail("bad map size");
            return;
        }
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            const auto key = in.pod<typename std::decay_t<decltype(target)>::key_type>();
            target.emplace(key, in.pod<Value>());
        }
    };
    std::unordered_map<int, EeAlarm> alarms;
    std::unordered_map<int, EeIrqHandler> intc, dmac;
    readPodMap(alarms, 4096u);
    readPodMap(intc, 4096u);
    readPodMap(dmac, 4096u);

    const int nextThreadId = in.i32(), nextInvocationThreadId = in.i32(), nextSemaphoreId = in.i32(), nextEventFlagId = in.i32(),
              nextAlarmId = in.i32();
    const uint64_t nextDelayToken = in.u64();
    const int nextIntc = in.i32(), nextDmac = in.i32(), intcHead = in.i32(), intcTail = in.i32(), dmacHead = in.i32(), dmacTail = in.i32();
    const uint32_t intcMask = in.u32(), dmacMask = in.u32();
    const bool reschedule = in.flag(), sliceExpired = in.flag();

    std::vector<ScheduledEvent> deadlines;
    std::deque<EeEvent> events;
    uint64_t eventSequence = 0;
    {
        const auto hostNow = std::chrono::steady_clock::now();
        const uint32_t n = in.u32();
        if (n > 65536u)
            in.fail("bad deadline count");
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            ScheduledEvent d;
            d.deadlineCycle = in.u64();
            const int64_t inNs = in.pod<int64_t>();
            d.hostDeadline = hostNow + std::chrono::nanoseconds(std::max<int64_t>(inNs, 0));
            d.event = in.pod<EeEvent>();
            d.sequence = in.u64();
            deadlines.push_back(d);
        }
        const uint32_t m = in.u32();
        if (m > 65536u)
            in.fail("bad event count");
        for (uint32_t i = 0; i < m && in.ok(); ++i)
            events.push_back(in.pod<EeEvent>());
        eventSequence = in.u64();
    }
    const uint64_t invocationSequence = in.u64();
    const uint64_t vsyncTick = in.u64();
    const uint32_t vsyncFlag = in.u32(), vsyncTickAddress = in.u32(), gsCallback = in.u32(), gsGp = in.u32(), gsSp = in.u32();
    std::unordered_map<uint64_t, uint32_t> stackTops;
    {
        const uint32_t n = in.u32();
        if (n > 65536u)
            in.fail("bad stack top count");
        for (uint32_t i = 0; i < n && in.ok(); ++i)
        {
            const uint64_t key = in.u64();
            stackTops.emplace(key, in.u32());
        }
    }
    std::vector<uint8_t> vu0Global;
    in.blob(vu0Global, 1u << 20);
    const bool vu0OwnerValid = in.flag();
    Vu0OwnerKey owner;
    owner.threadId = in.i32();
    owner.depth = static_cast<size_t>(in.u64());
    owner.sequence = in.u64();
    in.tag("EEKE");
    if (!in.ok())
        return false;

    // Everything read: replace the kernel's state at once.
    m_threads = std::move(threads);
    m_threadCacheId = -1;
    m_threadCache = nullptr;
    m_readyQueues = std::move(ready);
    m_semaphores = std::move(semaphores);
    m_semaphoreCacheId = -1;
    m_semaphoreCache = nullptr;
    m_eventFlags = std::move(flags);
    m_alarms = std::move(alarms);
    m_intcHandlers = std::move(intc);
    m_dmacHandlers = std::move(dmac);
    m_nextThreadId = nextThreadId;
    m_nextInvocationThreadId = nextInvocationThreadId;
    m_nextSemaphoreId = nextSemaphoreId;
    m_nextEventFlagId = nextEventFlagId;
    m_nextAlarmId = nextAlarmId;
    m_nextDelayToken = nextDelayToken;
    m_nextIntcHandlerId = nextIntc;
    m_nextDmacHandlerId = nextDmac;
    m_intcHeadOrder = intcHead;
    m_intcTailOrder = intcTail;
    m_dmacHeadOrder = dmacHead;
    m_dmacTailOrder = dmacTail;
    m_enabledIntcMask = intcMask;
    m_enabledDmacMask = dmacMask;
    m_currentThreadId = 0;
    m_rescheduleRequested = reschedule;
    m_timeSliceExpired = sliceExpired;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = timerIrqs;
    m_pendingInvocations.clear();
    m_invocationSequence = invocationSequence;
    m_vsyncTick = vsyncTick;
    m_vsyncFlagAddress = vsyncFlag;
    m_vsyncTickAddress = vsyncTickAddress;
    m_gsVSyncCallback = gsCallback;
    m_gsVSyncCallbackGp = gsGp;
    m_gsVSyncCallbackSp = gsSp;
    m_invocationStackTops = std::move(stackTops);
    m_vu0Global = std::move(vu0Global);
    m_vu0OwnerValid = vu0OwnerValid;
    m_vu0Owner = owner;

    // The clock: the saved EE time, the host clock rebased to it.
    m_clock.cycle = now;
#if !PS2X_EE_CLOCK64
    m_clock.run = 0;
    m_clock.limit = 0;
#endif
    m_accountedCycle = accounted;
    m_sliceEndCycle = sliceEnd;
    if (m_hostClockSync)
        m_hostClockBase = std::chrono::steady_clock::now() - std::chrono::microseconds(now * 1000000ull / kEeClockHz);
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines = std::move(deadlines);
        m_events = std::move(events);
        m_eventSequence = eventSequence;
        updateNextDeadline();
    }
    m_clock.pending.store(true, std::memory_order_release);
    return true;
}
