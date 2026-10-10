// The EE kernel calls of eekernel.h on Windows fibers; see kernel.h.

#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <process.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

// Last: on the PC it renames the EE calls that Windows also has (CreateThread).
#include <eekernel.h>

namespace pc {
namespace ee {

namespace {

const int kMaxThreads = 256;
const int kMaxSemas = 256;
const int kPriorities = 128;
// The game gives its threads 4 KiB stacks sized for the EE; host code (the
// C library, SDL, the renderer) needs far more.
const SIZE_T kFiberStack = 1024 * 1024;

const int kIntcVBlankStart = 2;
const int kIntcVBlankEnd = 3;

enum class State { Free, Dormant, Ready, Waiting };

struct Thread {
    State  state = State::Free;
    int    priority = 0;
    void (*entry)(void *) = nullptr;
    void  *arg = nullptr;
    LPVOID fiber = nullptr;
    bool   fiber_done = false; // entry returned; the fiber waits to be deleted
};

struct Sema {
    bool            used = false;
    int             count = 0;
    int             max = 0;
    std::deque<int> waiters;
};

struct Handler {
    int cause;
    int (*func)(int);
    int id;
};

struct Kernel {
    CRITICAL_SECTION lock;
    Thread           threads[kMaxThreads];
    Sema             semas[kMaxSemas];
    std::deque<int>  ready[kPriorities];
    int              current = 1;
    std::vector<int> retired; // threads whose fibers ended

    std::vector<Handler> intc;
    std::vector<Handler> dmac;
    int                  next_handler = 1;
    int (*vsync_callback)(int) = nullptr;
    void (*vblank_hook)() = nullptr;

    HANDLE                main_thread = nullptr;
    HANDLE                wake = nullptr;     // set after every interrupt
    HANDLE                stop = nullptr;
    HANDLE                clock = nullptr;    // the interrupt thread
    std::atomic<uint32_t> vblanks{0};
    LONGLONG              field_ns = 20000000;
    int                   dintr_depth = 0;
    int                   lock_depth = 0; // times the main thread holds `lock`
};

Kernel &K() {
    static Kernel kernel;
    return kernel;
}

void Acquire() {
    EnterCriticalSection(&K().lock);
    K().lock_depth++;
}

void Release() {
    K().lock_depth--;
    LeaveCriticalSection(&K().lock);
}

/** Holds the lock `depth` times, as a thread being resumed held it when it switched away. */
void SetLockDepth(int depth) {
    while (K().lock_depth < depth) {
        Acquire();
    }
    while (K().lock_depth > depth) {
        Release();
    }
}

struct Lock {
    Lock() { Acquire(); }
    ~Lock() { Release(); }
};

bool ValidThread(int id) { return id > 0 && id < kMaxThreads && K().threads[id].state != State::Free; }

void RemoveReady(int id) {
    Kernel &k = K();
    auto   &q = k.ready[k.threads[id].priority];
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (*it == id) {
            q.erase(it);
            return;
        }
    }
}

void MakeReady(int id) {
    Kernel &k = K();
    k.threads[id].state = State::Ready;
    k.ready[k.threads[id].priority].push_back(id);
}

int HighestReady() {
    Kernel &k = K();
    for (int p = 0; p < kPriorities; p++) {
        if (!k.ready[p].empty()) {
            return k.ready[p].front();
        }
    }
    return 0;
}

void DeleteRetired() {
    Kernel &k = K();
    for (size_t i = 0; i < k.retired.size();) {
        int id = k.retired[i];
        if (id != k.current) {
            Thread &t = k.threads[id];
            if (t.fiber != nullptr) {
                DeleteFiber(t.fiber);
                t.fiber = nullptr;
            }
            k.retired.erase(k.retired.begin() + long(i));
        } else {
            i++;
        }
    }
}

/**
 * Runs whichever thread should run now. Returns once the calling thread is
 * the one picked again. Called with the lock held; the lock is per host
 * thread, and every fiber shares the main thread, so it stays held across
 * the switch.
 */
void Reschedule() {
    Kernel &k = K();
    for (;;) {
        int next = HighestReady();
        if (next == 0) {
            // Every thread waits: let interrupts in and sleep until one has run.
            int depth = k.lock_depth;
            SetLockDepth(0);
            WaitForSingleObject(k.wake, 100);
            SetLockDepth(depth);
            continue;
        }
        if (next == k.current) {
            DeleteRetired();
            return;
        }
        Thread &t = k.threads[next];
        int     depth = k.lock_depth;
        k.current = next;
        SwitchToFiber(t.fiber);
        // Back on the calling thread's fiber, with the lock as the thread that
        // switched here left it.
        SetLockDepth(depth);
        DeleteRetired();
        if (k.threads[k.current].state == State::Ready && HighestReady() == k.current) {
            return;
        }
    }
}

void WINAPI FiberMain(LPVOID param) {
    int     id = int(intptr_t(param));
    Kernel &k = K();
    for (;;) {
        Thread &t = k.threads[id];
        SetLockDepth(0); // a new thread starts with interrupts enabled
        t.entry(t.arg);
        Acquire();
        // ExitThread: the thread goes back to DORMANT and can be started again.
        RemoveReady(id);
        t.state = State::Dormant;
        Reschedule();
        // Started again: Reschedule only returns here when picked.
    }
}

void RunHandlers(std::vector<Handler> &list, int cause) {
    for (const Handler &h : list) {
        if (h.cause == cause) {
            h.func(cause);
        }
    }
}

unsigned __stdcall ClockThread(void *) {
    Kernel &k = K();
    timeBeginPeriod(1);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    double   ticks_per_ns = double(freq.QuadPart) / 1e9;
    LONGLONG next = now.QuadPart + LONGLONG(double(k.field_ns) * ticks_per_ns);
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG wait = next - now.QuadPart;
        if (wait > 0) {
            DWORD ms = DWORD(double(wait) / ticks_per_ns / 1e6);
            if (WaitForSingleObject(k.stop, ms) == WAIT_OBJECT_0) {
                break;
            }
            QueryPerformanceCounter(&now);
            while (now.QuadPart < next) {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            }
        } else if (WaitForSingleObject(k.stop, 0) == WAIT_OBJECT_0) {
            break;
        }
        next += LONGLONG(double(k.field_ns) * ticks_per_ns);
        QueryPerformanceCounter(&now);
        if (now.QuadPart > next + LONGLONG(1e9 * ticks_per_ns)) {
            next = now.QuadPart; // stopped in a debugger: don't catch up a second of fields
        }

        // The interrupt: the main thread stops between two instructions, as
        // long as it is not inside the kernel or between DIntr and EIntr.
        EnterCriticalSection(&k.lock);
        SuspendThread(k.main_thread);
        CONTEXT context;
        context.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(k.main_thread, &context); // waits for the suspension to take hold
        k.vblanks.fetch_add(1);
        RunHandlers(k.intc, kIntcVBlankStart);
        if (k.vsync_callback != nullptr) {
            k.vsync_callback(0);
        }
        if (k.vblank_hook != nullptr) {
            k.vblank_hook();
        }
        RunHandlers(k.intc, kIntcVBlankEnd);
        ResumeThread(k.main_thread);
        LeaveCriticalSection(&k.lock);
        SetEvent(k.wake);
    }
    timeEndPeriod(1);
    return 0;
}

} // namespace

void KernelStart(bool pal) {
    Kernel &k = K();
    if (k.main_thread != nullptr) {
        return;
    }
    InitializeCriticalSection(&k.lock);
    k.field_ns = pal ? 20000000 : 16683350;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &k.main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    k.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    k.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    Thread &main = k.threads[1];
    main.state = State::Ready;
    main.priority = 1;
    main.fiber = ConvertThreadToFiber(nullptr);
    if (main.fiber == nullptr) {
        std::fprintf(stderr, "ee: ConvertThreadToFiber failed (%lu)\n", GetLastError());
        std::abort();
    }
    k.ready[main.priority].push_back(1);
    k.current = 1;
    k.clock = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ClockThread, nullptr, 0, nullptr));
    SetThreadPriority(k.clock, THREAD_PRIORITY_TIME_CRITICAL);
}

void KernelShutdown() {
    Kernel &k = K();
    if (k.clock != nullptr) {
        SetEvent(k.stop);
        WaitForSingleObject(k.clock, INFINITE);
        CloseHandle(k.clock);
        k.clock = nullptr;
    }
}

uint32_t VBlankCount() { return K().vblanks.load(); }

void WaitVBlankAfter(uint32_t count) {
    Kernel &k = K();
    while (int32_t(k.vblanks.load() - count) <= 0) {
        WaitForSingleObject(k.wake, 100);
    }
}

void SetVSyncCallback(int (*callback)(int)) {
    Lock lock;
    K().vsync_callback = callback;
}

void SetVBlankHook(void (*hook)()) {
    Lock lock;
    K().vblank_hook = hook;
}

void RaiseDmac(int channel) {
    Lock lock;
    RunHandlers(K().dmac, channel);
}

} // namespace ee
} // namespace pc

using namespace pc::ee;

extern "C" {

// Only its address is taken, for ThreadParam.gpReg.
void *_gp;

int CreateThread(struct ThreadParam *param) {
    Lock    lock;
    Kernel &k = K();
    for (int id = 2; id < kMaxThreads; id++) {
        Thread &t = k.threads[id];
        if (t.state != State::Free) {
            continue;
        }
        t = Thread();
        t.state = State::Dormant;
        t.priority = param->initPriority & (kPriorities - 1);
        t.entry = param->entry;
        return id;
    }
    return -1;
}

int StartThread(int thread_id, void *arg) {
    Lock    lock;
    Kernel &k = K();
    if (!ValidThread(thread_id) || k.threads[thread_id].state != State::Dormant) {
        return -1;
    }
    Thread &t = k.threads[thread_id];
    t.arg = arg;
    if (t.fiber == nullptr) {
        t.fiber = CreateFiberEx(kFiberStack, kFiberStack, 0, FiberMain, reinterpret_cast<LPVOID>(intptr_t(thread_id)));
        if (t.fiber == nullptr) {
            return -1;
        }
    }
    MakeReady(thread_id);
    Reschedule();
    return thread_id;
}

int TerminateThread(int thread_id) {
    Lock    lock;
    Kernel &k = K();
    if (!ValidThread(thread_id) || thread_id == k.current) {
        return -1;
    }
    Thread &t = k.threads[thread_id];
    if (t.state == State::Ready) {
        RemoveReady(thread_id);
    }
    for (Sema &s : k.semas) {
        for (auto it = s.waiters.begin(); it != s.waiters.end();) {
            it = *it == thread_id ? s.waiters.erase(it) : it + 1;
        }
    }
    t.state = State::Dormant;
    if (t.fiber != nullptr) {
        DeleteFiber(t.fiber);
        t.fiber = nullptr;
    }
    return thread_id;
}

int DeleteThread(int thread_id) {
    Lock    lock;
    Kernel &k = K();
    if (!ValidThread(thread_id) || k.threads[thread_id].state != State::Dormant) {
        return -1;
    }
    k.threads[thread_id].state = State::Free;
    return thread_id;
}

int GetThreadId(void) { return K().current; }

int ChangeThreadPriority(int thread_id, int priority) {
    Lock    lock;
    Kernel &k = K();
    if (thread_id == 0) {
        thread_id = k.current;
    }
    if (!ValidThread(thread_id)) {
        return -1;
    }
    Thread &t = k.threads[thread_id];
    int     old = t.priority;
    if (t.state == State::Ready) {
        RemoveReady(thread_id);
        t.priority = priority & (kPriorities - 1);
        k.ready[t.priority].push_back(thread_id);
    } else {
        t.priority = priority & (kPriorities - 1);
    }
    Reschedule();
    return old;
}

int RotateThreadReadyQueue(int priority) {
    Lock    lock;
    Kernel &k = K();
    auto   &q = k.ready[priority & (kPriorities - 1)];
    if (q.size() > 1) {
        int head = q.front();
        q.pop_front();
        q.push_back(head);
    }
    Reschedule();
    return 0;
}

int CreateSema(struct SemaParam *param) {
    Lock    lock;
    Kernel &k = K();
    for (int id = 1; id < kMaxSemas; id++) {
        Sema &s = k.semas[id];
        if (!s.used) {
            s = Sema();
            s.used = true;
            s.count = param->initCount;
            s.max = param->maxCount;
            return id;
        }
    }
    return -1;
}

int DeleteSema(int sema_id) {
    Lock    lock;
    Kernel &k = K();
    if (sema_id <= 0 || sema_id >= kMaxSemas || !k.semas[sema_id].used) {
        return -1;
    }
    Sema &s = k.semas[sema_id];
    for (int id : s.waiters) {
        MakeReady(id);
    }
    s = Sema();
    Reschedule();
    return sema_id;
}

int WaitSema(int sema_id) {
    Lock    lock;
    Kernel &k = K();
    if (sema_id <= 0 || sema_id >= kMaxSemas || !k.semas[sema_id].used) {
        return -1;
    }
    Sema &s = k.semas[sema_id];
    if (s.count > 0) {
        s.count--;
        return sema_id;
    }
    int self = k.current;
    RemoveReady(self);
    k.threads[self].state = State::Waiting;
    s.waiters.push_back(self);
    Reschedule();
    return sema_id;
}

int SignalSema(int sema_id) {
    Lock    lock;
    Kernel &k = K();
    if (sema_id <= 0 || sema_id >= kMaxSemas || !k.semas[sema_id].used) {
        return -1;
    }
    Sema &s = k.semas[sema_id];
    if (!s.waiters.empty()) {
        int id = s.waiters.front();
        s.waiters.pop_front();
        MakeReady(id);
        Reschedule();
    } else {
        s.count++;
    }
    return sema_id;
}

int DIntr(void) {
    Kernel &k = K();
    Acquire();
    return k.dintr_depth++ == 0 ? 1 : 0;
}

int EIntr(...) {
    Kernel &k = K();
    if (k.dintr_depth > 0) {
        k.dintr_depth--;
        Release();
    }
    return 1;
}

int AddIntcHandler(int cause, int (*handler)(int), int next) {
    (void) next;
    Lock    lock;
    Kernel &k = K();
    int     id = k.next_handler++;
    k.intc.push_back({cause, handler, id});
    return id;
}

int RemoveIntcHandler(int cause, int handler_id) {
    Lock    lock;
    Kernel &k = K();
    for (auto it = k.intc.begin(); it != k.intc.end(); ++it) {
        if (it->cause == cause && it->id == handler_id) {
            k.intc.erase(it);
            return 0;
        }
    }
    return -1;
}

int EnableIntc(int cause) {
    (void) cause;
    return 1;
}

int AddDmacHandler(int channel, int (*handler)(int), int next) {
    (void) next;
    Lock    lock;
    Kernel &k = K();
    int     id = k.next_handler++;
    k.dmac.push_back({channel, handler, id});
    return id;
}

int RemoveDmacHandler(int channel, int handler_id) {
    Lock    lock;
    Kernel &k = K();
    for (auto it = k.dmac.begin(); it != k.dmac.end(); ++it) {
        if (it->cause == channel && it->id == handler_id) {
            k.dmac.erase(it);
            return 0;
        }
    }
    return -1;
}

int EnableDmac(int channel) {
    (void) channel;
    return 1;
}

int DisableDmac(int channel) {
    (void) channel;
    return 1;
}

// The PC has coherent caches; the DMA shims read memory directly.
void FlushCache(int operation) { (void) operation; }

void iFlushCache(int operation) { (void) operation; }

void iSyncDCache(void *start, void *end) {
    (void) start;
    (void) end;
}

void Exit(int status) {
    std::fflush(nullptr);
    std::exit(status);
}

void Exit__2(int status) { Exit(status); }

} // extern "C"
