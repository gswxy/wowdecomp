#include <cstddef>
#include <cstdint>
#include <cstring>
#include "Engine/Source/Base/Prop.h"
#include "Engine/Source/Event/EvtContext.h"
#include "Engine/Source/Event/EvtInput.h"
#include "Engine/Source/Event/EvtQueue.h"
#include "Engine/Source/Event/EvtSched.h"
#include "Engine/Source/Event/EvtThread.h"
#include "Engine/Source/Event/EvtTimer.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/OsCall.h"
#include "Engine/Source/Os/OsTcp.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

#if defined(WOWDECOMP_ARCH_64)
#include <cstdarg>
#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace Event {
    // STATIC: 0x00AC0DC0
    volatile long s_threadListContention = -1;

    // STATIC: 0x00AC0DC4
    TSList<EvtThread, TSGetLink<EvtThread> > s_threadList;

    // STATIC: 0x00B417C4
    EvtContext* s_currentEvtContext;

    // STATIC: 0x00B417C8
    uint32_t s_mainThread;

    // STATIC: 0x00B417CC
    int32_t s_originalThreadPriority;

    // STATIC: 0x00B417D0
    volatile long s_interactiveCount;

    // STATIC: 0x00B417D4
    uint32_t s_threadSlotCount;

    // STATIC: 0x00B417D8
    EvtThread** s_threadSlots;

    // STATIC: 0x00B417DC
    SCritSect* s_threadSlotCritsects;

    // STATIC: 0x00B417E0
    int32_t s_netServer;

    // STATIC: 0x00B417E8
    SCritSect s_threadListCritsect;

    // STATIC: 0x00B41810
    TSGrowableArray<SThread*> s_schedulerThreads;

    // STATIC: 0x00B41820
    SEvent s_startEvent(1, 0);

    // STATIC: 0x00B41824
    SEvent s_shutdownEvent(1, 0);
}

// VERIFY: 77804885 auto
// OFFSET: 0x0047D0A0
void CloseContext(EvtContext* context) {
    context->m_critsect.Enter();

    if (context->m_schedState == SCHEDSTATE_ACTIVE) {
        context->m_schedState = SCHEDSTATE_CLOSED;
    }

    context->m_critsect.Leave();
}

// VERIFY: db640a01 auto
// OFFSET: 0x0047D950
uint32_t STORMAPI WaitPumpingNetwork(void* param) {
    SSyncObject* event = static_cast<SSyncObject*>(param);

    while (event->Wait(0)) {
        OsNetPump(100);
    }

    return 0;
}

// VERIFY: 1ec768f9 manual
// OFFSET: 0x0047D990
void IEvtSchedulerShutdown() {
    Event::s_shutdownEvent.Set();

    if (Event::s_netServer) {
        return;
    }

    Event::s_threadListCritsect.Enter();

    for (uint32_t i = 0; i < Event::s_threadSlotCount; i++) {
        EvtThread* thread = Event::s_threadSlots[i];

        if (thread) {
            thread->m_wakeEvent.Set();
        }
    }

    Event::s_threadListCritsect.Leave();
}

// VERIFY: d9dbe259 auto
// OFFSET: 0x0047DB40
int32_t SynthesizeInitialize(EvtContext* context) {
    if (context->m_schedFlags & 1) {
        return 0;
    }

    context->m_schedFlags |= 1;
    context->m_schedLastIdle = OsGetAsyncTimeMs();
    IEvtQueueDispatch(context, EVENT_ID_INITIALIZE, nullptr);

    return 1;
}

// VERIFY: b9c7171f manual
// OFFSET: 0x0047DB70
void IEvtDispatchUpdate(EvtContext* context, uint32_t currTime, float elapsedSec) {
    context->m_critsect.Enter();
    int32_t closed = context->m_schedState == SCHEDSTATE_CLOSED;
    context->m_critsect.Leave();

    if (!closed) {
        EVENT_DATA_UPDATE data;
        data.m_elapsedSec = elapsedSec;
        data.m_time = currTime;

        IEvtQueueDispatch(context, EVENT_ID_UPDATE, &data);
    }
}

// VERIFY: 8e0d29f6 manual
// OFFSET: 0x0047DBC0
void SynthesizeIdle(EvtContext* context, uint32_t currTime, float elapsedSec) {
    context->m_critsect.Enter();
    int32_t closed = context->m_schedState == SCHEDSTATE_CLOSED;
    context->m_critsect.Leave();

    if (!closed) {
        if (context->m_schedFlags & 2) {
            context->m_schedFlags |= 4;
        }

        EVENT_DATA_IDLE data;
        data.m_elapsedSec = elapsedSec;
        data.m_time = currTime;

        IEvtQueueDispatch(context, EVENT_ID_IDLE, &data);
    }
}

// VERIFY: 9966efd0 manual
// OFFSET: 0x0047DC20
void SynthesizePoll(EvtContext* context) {
    context->m_critsect.Enter();
    int32_t closed = context->m_schedState == SCHEDSTATE_CLOSED;
    context->m_critsect.Leave();

    if (!closed) {
        IEvtQueueDispatch(context, EVENT_ID_POLL, nullptr);
    }
}

// VERIFY: 9ce6122c manual
// OFFSET: 0x0047DC50
void SynthesizePaint(EvtContext* context) {
    context->m_critsect.Enter();
    int32_t closed = context->m_schedState == SCHEDSTATE_CLOSED;
    context->m_critsect.Leave();

    if (!closed && (context->m_schedFlags & 4)) {
        context->m_schedFlags &= ~4u;
        IEvtQueueDispatch(context, EVENT_ID_PAINT, nullptr);
    }
}

// VERIFY: f0d6cd9f auto
// OFFSET: 0x0047DC90
int32_t IEvtSchedulerIsContextInteractive(EvtContext* context) {
    return (context->m_schedFlags >> 1) & 1;
}

// VERIFY: 17d819bf manual
// OFFSET: 0x0047DCA0
void EventForceIdleProcessing() {
    // STATIC: 0x00B417C0
    static int32_t s_watchdogStarted;

    EvtContext* context = Event::s_currentEvtContext;

    if (!context) {
        return;
    }

    PropSelectContext(context->m_propContext);
    PropSet(PROP_EVENTCONTEXT, reinterpret_cast<void*>(static_cast<uintptr_t>(context->m_id)));
    OsCallSetContext(context->m_callContext);

    uint32_t currTime = OsGetAsyncTimeMs();

    if (SynthesizeInitialize(context) && context->m_startWatchdog) {
        s_watchdogStarted = 1;
    }

    float elapsedSec = static_cast<float>(static_cast<int32_t>(currTime - context->m_schedLastIdle)) * 0.001f;
    context->m_schedLastIdle = currTime;

    IEvtDispatchUpdate(context, currTime, elapsedSec);
    IEvtTimerDispatch(context);
    SynthesizePoll(context);
    IEvtQueueDispatchAll(context);
    SynthesizeIdle(context, currTime, elapsedSec);
    SynthesizePaint(context);
}

// VERIFY: 0a77dcc6 manual
// OFFSET: 0x0047DEA0
void IEvtSchedulerDestroy() {
    if (!Event::s_threadSlotCount) {
        return;
    }

    SEvent shutdownEvent(1, 0);
    TSGrowableArray<SThread*> shutdownThreads;

    if (Event::s_netServer) {
        uint32_t processors = OsGetNumberOfProcessors();

        SThread* shutdownThread = new (SMemAlloc(sizeof(SThread), __FILE__, __LINE__, 0)) SThread;
        *shutdownThreads.New() = shutdownThread;

        while (processors) {
            processors--;

            SThread thread;

            if (!SThread::Create(WaitPumpingNetwork, &shutdownEvent, **shutdownThreads.Top(), const_cast<char*>("EvtShutdown"), 0)) {
                delete *shutdownThreads.Top();
                shutdownThreads.SetCount(shutdownThreads.Count() - 1);
            }
        }
    }

    GxTeardownLog("IEvtSchedulerDestroy: join scheduler threads");

    // Not stock (stock waits INFINITE): bounded so a stuck scheduler thread cannot hang the quit. On timeout the
    // thread tables are leaked, not freed, as the stuck thread may still use them.
    if (SSyncObject::WaitMultiplePtr(Event::s_schedulerThreads.Count(), reinterpret_cast<SSyncObject**>(Event::s_schedulerThreads.Ptr()), 1, 2000) == 0x102 /* WAIT_TIMEOUT */) {
        GxTeardownLog("IEvtSchedulerDestroy: scheduler threads did not exit in 2 s, skipping the rest");
        return;
    }

    GxTeardownLog("IEvtSchedulerDestroy: scheduler threads joined");

    if (shutdownThreads.Count()) {
        shutdownEvent.Set();
        SSyncObject::WaitMultiplePtr(shutdownThreads.Count(), reinterpret_cast<SSyncObject**>(shutdownThreads.Ptr()), 1, 0xFFFFFFFF);

        for (uint32_t i = shutdownThreads.Count(); i; i--) {
            delete shutdownThreads[i - 1];
        }
    }

    for (uint32_t i = Event::s_schedulerThreads.Count(); i; i--) {
        delete Event::s_schedulerThreads[i - 1];
    }

    Event::s_schedulerThreads.Clear();

    delete[] Event::s_threadSlots;
    Event::s_threadSlots = nullptr;

    delete[] Event::s_threadSlotCritsects;
    Event::s_threadSlotCritsects = nullptr;

    Event::s_threadSlotCount = 0;
}

// VERIFY: 6432e730 manual
// OFFSET: 0x0047E2F0
void SynthesizeDestroy(EvtContext* context) {
    SynthesizeInitialize(context);

    if ((context->m_schedFlags & 2) && !InterlockedDecrement(&Event::s_interactiveCount)) {
        IEvtSchedulerShutdown();
    }

    IEvtQueueDispatch(context, EVENT_ID_3, nullptr);

    context->m_critsect.Enter();
    context->m_schedState = SCHEDSTATE_DESTROYED;
    context->m_critsect.Leave();

    IEvtQueueDispatchAll(context);
    IEvtQueueDispatch(context, EVENT_ID_DESTROY, nullptr);

    OsCallResetContext(context->m_callContext);
    PropSelectContext(nullptr);

    int32_t findMask;
    TSingletonInstanceId<EvtContext, 8>::s_idTable.Ptr(context->m_id, 1, &findMask, 0, 1);

    delete context;

    if (findMask != -1) {
        TSingletonInstanceId<EvtContext, 8>::s_idTable.Unlock(findMask & 7, findMask >= 8);
    }
}

// VERIFY: f62020ab manual
// OFFSET: 0x0047E5A0
EvtContext* GetNextContext(uint32_t hThread) {
    Event::s_threadSlotCritsects[hThread].Enter();
    EvtContext* context = Event::s_threadSlots[hThread]->m_contextQueue.Dequeue();
    Event::s_threadSlotCritsects[hThread].Leave();

    if (hThread == Event::s_mainThread) {
        Event::s_currentEvtContext = context;
    }

    return context;
}

// VERIFY: 1a6d7021 manual
// OFFSET: 0x0047E910
uint32_t InitializeSchedulerThread() {
    InterlockedIncrement(&Event::s_threadListContention);
    Event::s_threadListCritsect.Enter();

    uint32_t slot = Event::s_threadSlotCount;

    for (uint32_t i = 0; i < Event::s_threadSlotCount; i++) {
        if (slot == Event::s_threadSlotCount || !Event::s_threadSlots[i] || Event::s_threadSlots[i]->m_threadCount < Event::s_threadSlots[slot]->m_threadCount) {
            slot = i;

            if (!Event::s_threadSlots[i]) {
                break;
            }
        }
    }

    EvtThread* thread = Event::s_threadSlots[slot];

    if (!thread) {
        thread = Event::s_threadList.NewNode(2, 0, 0);
        thread->m_threadCount = 0;
        thread->m_weightTotal = 0;
        thread->m_weightAvg = 0;
        thread->m_contextCount = 0;
        thread->m_rebalance = 0;
        thread->m_threadSlot = slot;

        Event::s_threadSlotCritsects[slot].Enter();
        Event::s_threadSlots[slot] = thread;
        Event::s_threadSlotCritsects[slot].Leave();
    }

    thread->m_threadCount++;

    Event::s_threadListCritsect.Leave();
    InterlockedDecrement(&Event::s_threadListContention);

    return slot;
}

// VERIFY: 9941be06 manual
// OFFSET: 0x0047EA70
void DestroySchedulerThread(uint32_t hThread) {
    TSGrowableArray<EvtContext*> contexts;

    InterlockedIncrement(&Event::s_threadListContention);
    Event::s_threadListCritsect.Enter();

    EvtThread* thread = Event::s_threadSlots[hThread];

    if (thread && --thread->m_threadCount == 0) {
        Event::s_threadSlotCritsects[hThread].Enter();
        Event::s_threadSlots[hThread] = nullptr;
        Event::s_threadSlotCritsects[hThread].Leave();

        contexts.Reserve(thread->m_contextQueue.Count(), 0);

        EvtContext* context;

        while ((context = thread->m_contextQueue.Dequeue()) != nullptr) {
            *contexts.New() = context;
        }

        Event::s_threadList.DeleteNode(thread);
    }

    Event::s_threadListCritsect.Leave();
    InterlockedDecrement(&Event::s_threadListContention);

    for (uint32_t i = contexts.Count(); i; i--) {
        EvtContext* context = contexts[i - 1];

        CloseContext(context);

        PropSelectContext(context->m_propContext);
        PropSet(PROP_EVENTCONTEXT, reinterpret_cast<void*>(static_cast<uintptr_t>(context->m_id)));
        OsCallSetContext(context->m_callContext);

        SynthesizeDestroy(context);
    }
}

// VERIFY: 534e2696 manual
// OFFSET: 0x0047EC10
HEVENTCONTEXT AttachContextToThread(EvtContext* context) {
    HEVENTCONTEXT handle = nullptr;

    InterlockedIncrement(&Event::s_threadListContention);
    Event::s_threadListCritsect.Enter();

    EvtThread* found = nullptr;

    for (EvtThread* thread = Event::s_threadList.Head(); thread; thread = Event::s_threadList.Next(thread)) {
        if (!found || thread->m_weightTotal < found->m_weightTotal) {
            found = thread;
        }
    }

    if (found) {
        handle = reinterpret_cast<HEVENTCONTEXT>(static_cast<uintptr_t>(TSingletonInstanceId<EvtContext, 8>::s_idTable.Insert(context)));

        context->m_schedNextWakeTime.SetVal(OsGetAsyncTimeMs());

        Event::s_threadSlotCritsects[found->m_threadSlot].Enter();
        found->m_contextQueue.Enqueue(context);
        Event::s_threadSlotCritsects[found->m_threadSlot].Leave();

        found->m_wakeEvent.Set();

        found->m_weightTotal += context->m_schedWeight;
        found->m_contextCount++;
        found->m_weightAvg = found->m_weightTotal / found->m_contextCount;
    } else if (context) {
        delete context;
    }

    Event::s_threadListCritsect.Leave();
    InterlockedDecrement(&Event::s_threadListContention);

    return handle;
}

// VERIFY: 06f63a78 manual
// OFFSET: 0x0047ED30
void DetachContextFromThread(uint32_t hThread, EvtContext* context) {
    InterlockedIncrement(&Event::s_threadListContention);
    Event::s_threadListCritsect.Enter();

    EvtThread* thread = Event::s_threadSlots[hThread];

    if (thread) {
        thread->m_weightTotal -= context->m_schedWeight;

        if (--thread->m_contextCount) {
            thread->m_weightAvg = thread->m_weightTotal / thread->m_contextCount;
        } else {
            thread->m_weightAvg = 0;
        }

        for (EvtThread* other = Event::s_threadList.Head(); other; other = Event::s_threadList.Next(other)) {
            if (other != thread && other->m_weightAvg && other->m_weightTotal >= other->m_weightAvg + thread->m_weightTotal) {
                other->m_rebalance += (other->m_weightTotal - thread->m_weightTotal) / other->m_weightAvg;
                other->m_rebalance = other->m_rebalance < other->m_contextCount ? other->m_rebalance : other->m_contextCount;
            }
        }
    }

    Event::s_threadListCritsect.Leave();
    InterlockedDecrement(&Event::s_threadListContention);
}

// VERIFY: 86cbefd2 manual
// OFFSET: 0x0047EDF0
void PutContext(uint32_t nextWakeTime, uint32_t newSmoothWeight, EvtContext* context, uint32_t hThread) {
    context->m_schedNextWakeTime.SetVal(nextWakeTime);

    if (newSmoothWeight != context->m_schedSmoothWeight) {
        uint32_t weight = context->m_schedWeight;
        context->m_schedSmoothWeight = newSmoothWeight;

        uint32_t delta = newSmoothWeight > weight ? newSmoothWeight - weight : weight - newSmoothWeight;
        context->m_schedRebalance = delta >= (weight >> 3);
    }

    uint32_t slot = hThread;

    if (InterlockedIncrement(&Event::s_threadListContention) == 0) {
        Event::s_threadListCritsect.Enter();

        EvtThread* thread = Event::s_threadSlots[hThread];

        if (thread->m_rebalance || context->m_schedRebalance) {
            if (context->m_schedRebalance) {
                thread->m_weightTotal -= context->m_schedWeight;
                context->m_schedWeight = context->m_schedSmoothWeight;
                thread->m_weightTotal += context->m_schedWeight;
            }

            EvtThread* found = thread;
            uint32_t foundWeight = thread->m_weightTotal;

            for (EvtThread* other = Event::s_threadList.Head(); other; other = Event::s_threadList.Next(other)) {
                if (other != thread && other->m_weightTotal + context->m_schedWeight < foundWeight) {
                    found = other;
                    foundWeight = other->m_weightTotal + context->m_schedWeight;
                }
            }

            if (found == thread) {
                if (context->m_schedRebalance) {
                    context->m_schedRebalance = 0;
                    thread->m_weightAvg = thread->m_weightTotal / thread->m_contextCount;
                }

                if (context->m_schedWeight <= thread->m_weightAvg) {
                    thread->m_rebalance = 0;
                }
            } else {
                thread->m_weightTotal -= context->m_schedWeight;
                thread->m_contextCount--;
                thread->m_weightAvg = thread->m_weightTotal / thread->m_contextCount;

                found->m_contextCount++;
                found->m_weightTotal = foundWeight;
                found->m_weightAvg = foundWeight / found->m_contextCount;

                context->m_schedRebalance = 0;

                if (thread->m_rebalance) {
                    thread->m_rebalance--;
                }

                Event::s_threadSlotCritsects[found->m_threadSlot].Enter();
                found->m_contextQueue.Enqueue(context);
                Event::s_threadSlotCritsects[found->m_threadSlot].Leave();

                found->m_wakeEvent.Set();

                slot = Event::s_threadSlotCount;
            }
        }

        Event::s_threadListCritsect.Leave();
    }

    InterlockedDecrement(&Event::s_threadListContention);

    if (slot < Event::s_threadSlotCount) {
        Event::s_threadSlotCritsects[slot].Enter();
        Event::s_threadSlots[slot]->m_contextQueue.Enqueue(context);
        Event::s_threadSlotCritsects[slot].Leave();
    }
}

// VERIFY: 6f17c9b6 manual
// OFFSET: 0x0047EFF0
int32_t SchedulerThreadProcProcess(uint32_t hThread, int32_t* watchdogStarted) {
    if (SGetCurrentThreadPriority() != Event::s_originalThreadPriority) {
        SSetCurrentThreadPriority(Event::s_originalThreadPriority);
    }

    if (!Event::s_shutdownEvent.Wait(0)) {
        return 1;
    }

    EvtContext* context = GetNextContext(hThread);
    int32_t timeout = -1;

    if (context) {
        timeout = context->m_schedNextWakeTime.m_val - OsGetAsyncTimeMs();

        if (timeout < 0) {
            timeout = 0;
        }
    }

    uint32_t result;

    if (Event::s_netServer) {
        if (timeout == -1) {
            timeout = 100;
        }

        OsNetPump(timeout);
        result = 258;
    } else {
        result = Event::s_threadSlots[hThread]->m_wakeEvent.Wait(timeout);
    }

    if (!context) {
        return 0;
    }

    PropSelectContext(context->m_propContext);
    PropSet(PROP_EVENTCONTEXT, reinterpret_cast<void*>(static_cast<uintptr_t>(context->m_id)));
    OsCallSetContext(context->m_callContext);

    uint32_t currTime = OsGetAsyncTimeMs();

    if (result == 258) {
        if (SynthesizeInitialize(context) && context->m_startWatchdog) {
            *watchdogStarted = 1;
        }

        float elapsedSec = static_cast<float>(static_cast<int32_t>(currTime - context->m_schedLastIdle)) * 0.001f;
        context->m_schedLastIdle = currTime;

        IEvtDispatchUpdate(context, currTime, elapsedSec);
        IEvtTimerDispatch(context);

        if (context->m_schedFlags & 2) {
            int32_t shutdown = 0;
            IEvtInputProcess(context, &shutdown);

            if (shutdown) {
                CloseContext(context);
                IEvtSchedulerShutdown();
            }
        }

        SynthesizePoll(context);
        IEvtQueueDispatchAll(context);
        SynthesizeIdle(context, currTime, elapsedSec);
        SynthesizePaint(context);
    }

    if (hThread == Event::s_mainThread) {
        Event::s_currentEvtContext = nullptr;
    }

    context->m_critsect.Enter();
    int32_t closed = context->m_schedState == SCHEDSTATE_CLOSED;
    context->m_critsect.Leave();

    if (closed) {
        DetachContextFromThread(hThread, context);
        SynthesizeDestroy(context);
        return 0;
    }

    uint32_t delay;

    if (context->m_schedFlags & 4) {
        delay = 0;
    } else {
        delay = IEvtTimerGetNextTime(context, currTime);

        if (context->m_schedIdleTime != context->m_schedInitialIdleTime) {
            delay = context->m_schedIdleTime;
        }

        int32_t idleDelay = context->m_schedLastIdle - currTime + context->m_schedIdleTime;

        if (idleDelay < 0) {
            idleDelay = 0;
        }

        if (delay >= static_cast<uint32_t>(idleDelay)) {
            delay = idleDelay;
        }
    }

    OsCallResetContext(context->m_callContext);
    PropSelectContext(nullptr);

    PutContext(currTime + delay, context->m_schedSmoothWeight, context, hThread);

    return 0;
}

// VERIFY: 1a5d245f auto
// OFFSET: 0x0047F230
uint32_t STORMAPI SchedulerThreadProc(void* mainThread) {
    uint32_t hThread;

    if (mainThread) {
        hThread = Event::s_mainThread;
    } else {
        hThread = InitializeSchedulerThread();
    }

    int32_t watchdogStarted = 0;

    PropSelectContext(nullptr);
    Event::s_startEvent.Wait(0xFFFFFFFF);

    char name[64];
    SStrPrintf(name, sizeof(name), "Engine %x", SGetCurrentThreadId());
    OsCallInitialize(name);

    while (!SchedulerThreadProcProcess(hThread, &watchdogStarted)) {
#if defined(WOWDECOMP_ARCH_64)
        // Not stock (hangdiag): main-loop heartbeat for the hang watchdog
        if (mainThread) {
            HangWatchdogTick();
        }
#endif
    }

    DestroySchedulerThread(hThread);
    OsCallDestroy();

    return 0;
}

#if defined(WOWDECOMP_ARCH_64)
// Not stock (hangdiag): if the main loop stops ticking for HANG_LIMIT_MS, a watchdog thread suspends every other thread
// of the process in turn, walks its stack with the x64 unwind tables and writes module+RVA frames to Logs\hang.txt
// (resolve offline against Wow64.map). Logs once per hang, and once more when the main loop recovers.
static const DWORD HANG_LIMIT_MS = 4000;
static const int32_t HANG_MAX_FRAMES = 48;
static volatile DWORD s_hangTick;
static DWORD s_hangMainThreadId;

void HangLog(const char* format, ...) {
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);

    while (length && path[length - 1] != '\\' && path[length - 1] != '/') {
        length--;
    }

    _snprintf_s(path + length, MAX_PATH - length, _TRUNCATE, "Logs");
    CreateDirectoryA(path, nullptr);
    _snprintf_s(path + length, MAX_PATH - length, _TRUNCATE, "Logs\\hang.txt");

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2048];
    int32_t used = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, format);
    _vsnprintf_s(line + used, sizeof(line) - used - 2, _TRUNCATE, format, args);
    va_end(args);
    strcat_s(line, sizeof(line), "\r\n");

    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(file);
    }
}

// Runs while the thread is suspended: no heap, no locks, only reads of its stack.
static int32_t HangCaptureStack(CONTEXT* context, DWORD64* frames) {
    int32_t count = 0;

    __try {
        while (count < HANG_MAX_FRAMES && context->Rip) {
            frames[count++] = context->Rip;

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context->Rip, &imageBase, nullptr);

            if (!function) {
                context->Rip = *reinterpret_cast<DWORD64*>(context->Rsp);
                context->Rsp += 8;
                continue;
            }

            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context->Rip, function, context, &handlerData, &establisher, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    return count;
}

static void HangDumpThread(DWORD threadId) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);

    if (!thread) {
        return;
    }

    DWORD64 frames[HANG_MAX_FRAMES];
    int32_t count = 0;

    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_FULL;

        if (GetThreadContext(thread, &context)) {
            count = HangCaptureStack(&context, frames);
        }

        ResumeThread(thread);
    }

    CloseHandle(thread);

    HangLog("  thread %lu%s, %d frames", threadId, threadId == s_hangMainThreadId ? " (MAIN)" : "", count);

    for (int32_t i = 0; i < count; i++) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(frames[i]), &module)) {
            char path[MAX_PATH];

            if (GetModuleFileNameA(module, path, MAX_PATH)) {
                const char* base = strrchr(path, '\\');
                strcpy_s(name, sizeof(name), base ? base + 1 : path);
            }
        }

        HangLog("    #%02d %s+0x%llX", i, name, static_cast<unsigned long long>(frames[i] - reinterpret_cast<DWORD64>(module)));
    }
}

static DWORD WINAPI HangWatchdogThread(void*) {
    int32_t reported = 0;
    DWORD reportedTick = 0;

    while (true) {
        Sleep(500);

        DWORD tick = s_hangTick;
        DWORD stalled = GetTickCount() - tick;

        if (reported) {
            if (tick != reportedTick) {
                HangLog("main loop recovered after a %lu ms stall", tick - reportedTick);
                reported = 0;
            }

            continue;
        }

        if (stalled < HANG_LIMIT_MS) {
            continue;
        }

        reported = 1;
        reportedTick = tick;
        HangLog("HANG: main loop has not ticked for %lu ms; stacks follow (MAIN first)", stalled);
        HangDumpThread(s_hangMainThreadId);

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);

            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId() && entry.th32ThreadID != s_hangMainThreadId) {
                    HangDumpThread(entry.th32ThreadID);
                }
            }

            CloseHandle(snapshot);
        }

        HangLog("HANG: end of stacks");
    }
}

void HangWatchdogTick() {
    s_hangTick = GetTickCount();

    if (!s_hangMainThreadId) {
        s_hangMainThreadId = GetCurrentThreadId();
        HANDLE thread = CreateThread(nullptr, 0, HangWatchdogThread, nullptr, 0, nullptr);

        if (thread) {
            CloseHandle(thread);
        }
    }
}
#endif

// VERIFY: 1a6d8e94 auto
// OFFSET: 0x0047F2D0
void IEvtSchedulerProcess() {
    Event::s_startEvent.Set();
    SchedulerThreadProc(reinterpret_cast<void*>(1));
    Event::s_mainThread = 0;
}

// VERIFY: b3031e06 manual
// OFFSET: 0x0047F2F0
void IEvtSchedulerInitialize(uint32_t threadCount, int32_t netServer) {
    if (Event::s_threadSlotCount) {
        DebugLog("IEvtScheduler already initialized");
    }

    Event::s_netServer = netServer;
    Event::s_originalThreadPriority = SGetCurrentThreadPriority();

    uint32_t slotCount = 1;

    while (slotCount < threadCount) {
        slotCount *= 2;

        if (!slotCount) {
            break;
        }
    }

    Event::s_threadSlotCount = slotCount;
    Event::s_threadSlotCritsects = new (__FILE__, __LINE__) SCritSect[slotCount];
    Event::s_threadSlots = new (__FILE__, __LINE__) EvtThread*[Event::s_threadSlotCount];
    memset(Event::s_threadSlots, 0, Event::s_threadSlotCount * sizeof(EvtThread*));

    Event::s_startEvent.Reset();
    Event::s_shutdownEvent.Reset();

    Event::s_mainThread = InitializeSchedulerThread();

    for (uint32_t i = threadCount - 1; i; i--) {
        SThread* thread = new (SMemAlloc(sizeof(SThread), __FILE__, __LINE__, 0)) SThread;
        *Event::s_schedulerThreads.New() = thread;

        char name[16];
        SStrPrintf(name, sizeof(name), "EvtSched#%d", i);

        if (!SThread::Create(SchedulerThreadProc, nullptr, **Event::s_schedulerThreads.Top(), name, 0)) {
            delete *Event::s_schedulerThreads.Top();
            Event::s_schedulerThreads.SetCount(Event::s_schedulerThreads.Count() - 1);
        }
    }
}

// VERIFY: da7dda2f manual
// OFFSET: 0x0047F5C0
HEVENTCONTEXT IEvtSchedulerCreateContext(int32_t interactive, EVENTHANDLERFUNC initializeHandler, EVENTHANDLERFUNC destroyHandler, uint32_t idleTime, uint32_t debugFlags) {
    if (idleTime < 1) {
        idleTime = 1;
    }

    void* callContext = nullptr;

    if (debugFlags & 1) {
        char name[256];
        SStrPrintf(name, sizeof(name), "Context: interactive = %u, idleTime = %u", interactive, idleTime);
        callContext = OsCallInitializeContext(name);
    }

    EvtContext* context = new (SMemAlloc(sizeof(EvtContext), __FILE__, __LINE__, 0)) EvtContext(interactive ? 2 : 0, idleTime, interactive ? 1000 : 1, callContext, (debugFlags >> 1) & 1);

    if (interactive) {
        InterlockedIncrement(&Event::s_interactiveCount);
    }

    if (initializeHandler) {
        IEvtQueueRegister(context, EVENT_ID_INITIALIZE, initializeHandler, nullptr, 1000.0f);
    }

    if (destroyHandler) {
        IEvtQueueRegister(context, EVENT_ID_DESTROY, destroyHandler, nullptr, 1000.0f);
    }

    return AttachContextToThread(context);
}
