#include "crash_report.hpp"

#ifdef _WIN32

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <windows.h>

namespace {
using Clock = std::chrono::steady_clock;

constexpr int  kMaxFrames = 48;
constexpr auto kStallLimit = std::chrono::seconds(5);

std::atomic<std::int64_t> g_heartbeat{0};
std::atomic<bool>         g_game_thread_known{false};
HANDLE                    g_game_thread = nullptr;
DWORD                     g_game_thread_id = 0;

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void PrintAddress(DWORD64 address) {
    HMODULE module = nullptr;
    char    path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module != nullptr) {
        GetModuleFileNameA(module, path, MAX_PATH);
        const char *name = path;
        for (const char *p = path; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                name = p + 1;
            }
        }
        std::fprintf(stderr, "  %s+0x%llx\n", name,
                     static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::fprintf(stderr, "  0x%llx\n", static_cast<unsigned long long>(address));
    }
}

// Unwinds with the x64 function tables, staying inside the stack's own allocation. Takes no locks the game could
// hold, so it may run while the game thread is suspended; printing waits until it is resumed.
int CollectStack(CONTEXT context, DWORD64 *frames) {
    MEMORY_BASIC_INFORMATION stack = {};
    if (VirtualQuery(reinterpret_cast<void *>(context.Rsp), &stack, sizeof(stack)) == 0) {
        return 0;
    }
    const DWORD64 low = reinterpret_cast<DWORD64>(stack.BaseAddress);
    const DWORD64 high = low + stack.RegionSize;
    int           count = 0;
    while (count < kMaxFrames && context.Rip != 0) {
        frames[count++] = context.Rip;
        DWORD64 image_base = 0;
        auto   *entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (entry == nullptr) {
            // A leaf function keeps its return address at the top of the stack.
            if (context.Rsp < low || context.Rsp + 8 > high) {
                break;
            }
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void   *handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data, &establisher,
                         nullptr);
        if (context.Rsp < low || context.Rsp >= high) {
            break;
        }
    }
    return count;
}

void PrintFrames(const DWORD64 *frames, int count) {
    for (int i = 0; i < count; ++i) {
        PrintAddress(frames[i]);
    }
    std::fflush(stderr);
}

void PrintStack(const CONTEXT &context) {
    DWORD64 frames[kMaxFrames];
    PrintFrames(frames, CollectStack(context, frames));
}

void PrintCurrentStack() {
    CONTEXT context = {};
    RtlCaptureContext(&context);
    PrintStack(context);
}

LONG WINAPI UnhandledException(EXCEPTION_POINTERS *info) {
    std::fflush(stdout);
    const auto *record = info->ExceptionRecord;
    std::fprintf(stderr, "crash: exception 0x%08lx at", static_cast<unsigned long>(record->ExceptionCode));
    PrintAddress(reinterpret_cast<DWORD64>(record->ExceptionAddress));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        std::fprintf(stderr, "crash: %s address 0x%llx\n", record->ExceptionInformation[0] == 1 ? "write to" : "read of",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    const CONTEXT &c = *info->ContextRecord;
    std::fprintf(stderr, "crash: rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx\n",
                 c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp);
    std::fprintf(stderr, "crash: r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n", c.R8,
                 c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
    std::fprintf(stderr, "crash: stack\n");
    PrintStack(c);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() (an arena overflow, a failed assertion) never reaches the exception filter.
void AbortSignal(int) {
    std::fflush(stdout);
    std::fprintf(stderr, "crash: abort; stack:\n");
    PrintCurrentStack();
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

// Logs the game thread's stack once per stall: a loop that never reaches the pump leaves the window frozen.
void Watchdog() {
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_game_thread_known.load()) {
            continue;
        }
        const std::int64_t last = g_heartbeat.load();
        const bool         stalled = Now() - last > std::chrono::milliseconds(kStallLimit).count();
        if (!stalled) {
            reported = false;
            continue;
        }
        if (reported) {
            continue;
        }
        reported = true;
        if (SuspendThread(g_game_thread) == static_cast<DWORD>(-1)) {
            continue;
        }
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_FULL;
        DWORD64 frames[kMaxFrames];
        int     count = -1;
        if (GetThreadContext(g_game_thread, &context)) {
            count = CollectStack(context, frames);
        }
        ResumeThread(g_game_thread);
        if (count >= 0) {
            std::fprintf(stderr, "watchdog: the game has not reached the host pump for %lld ms; its stack:\n",
                         static_cast<long long>(Now() - last));
            PrintFrames(frames, count);
        }
    }
}
} // namespace

void CrashReportInstall() {
    SetUnhandledExceptionFilter(UnhandledException);
    std::signal(SIGABRT, AbortSignal);
    std::thread(Watchdog).detach();
}

void CrashReportHeartbeat() {
    g_heartbeat.store(Now());
    if (!g_game_thread_known.load()) {
        g_game_thread_id = GetCurrentThreadId();
        g_game_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   g_game_thread_id);
        g_game_thread_known.store(g_game_thread != nullptr);
    }
}

void CrashReportWindowClosed() {
    std::fflush(stdout);
    std::fprintf(stderr, "window: closed by the player; the game was here:\n");
    PrintCurrentStack();
}

#else

void CrashReportInstall() {}
void CrashReportHeartbeat() {}
void CrashReportWindowClosed() {}

#endif
