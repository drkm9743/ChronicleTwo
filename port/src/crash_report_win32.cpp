// Windows crash report: an unhandled exception prints its code, address and call stack to stderr
// and to crash_report.txt next to the executable, each frame as module+RVA so a build's symbols can
// name it afterwards (llvm-symbolizer --obj=darkchronicle.exe 0x140000000+RVA).

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace {

// What the game was doing when it crashed, set by the loaders (the file being read, its buffer and size).
char g_note[512] = "";
volatile LONG g_reports = 0;

void ModuleOf(DWORD64 address, wchar_t *name, DWORD size, DWORD64 *base) {
    HMODULE module = nullptr;
    name[0] = 0;
    *base = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module != nullptr) {
        wchar_t path[MAX_PATH];
        DWORD   length = GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t *file = path;
        for (DWORD i = 0; i < length; ++i) {
            if (path[i] == L'\\' || path[i] == L'/') {
                file = path + i + 1;
            }
        }
        std::swprintf(name, size, L"%ls", file);
        *base = reinterpret_cast<DWORD64>(module);
    }
}

void Report(FILE *out, EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    std::fprintf(out, "\n==== darkchronicle crash ====\nexception 0x%08lX at %p",
                 static_cast<unsigned long>(record->ExceptionCode), record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        std::fprintf(out, " (%s %p)", record->ExceptionInformation[0] == 0   ? "read"
                                      : record->ExceptionInformation[0] == 8 ? "execute"
                                                                             : "write",
                     reinterpret_cast<void *>(record->ExceptionInformation[1]));
    }
    std::fprintf(out, "\nthread %lu\n", static_cast<unsigned long>(GetCurrentThreadId()));
    const CONTEXT &regs = *info->ContextRecord;
    std::fprintf(out, "rax %016llX rbx %016llX rcx %016llX rdx %016llX\nrsi %016llX rdi %016llX rsp %016llX rbp %016llX\n"
                      "r8  %016llX r9  %016llX r14 %016llX r15 %016llX\n",
                 regs.Rax, regs.Rbx, regs.Rcx, regs.Rdx, regs.Rsi, regs.Rdi, regs.Rsp, regs.Rbp, regs.R8, regs.R9,
                 regs.R14, regs.R15);
    if (g_note[0] != 0) {
        std::fprintf(out, "last read: %s\n", g_note);
    }

    CONTEXT context = *info->ContextRecord;
    for (int frame = 0; frame < 64 && context.Rip != 0; ++frame) {
        wchar_t name[MAX_PATH];
        DWORD64 base = 0;
        ModuleOf(context.Rip, name, MAX_PATH, &base);
        std::fprintf(out, "#%02d %ls+0x%llX (%p)\n", frame, name[0] ? name : L"?",
                     static_cast<unsigned long long>(context.Rip - base), reinterpret_cast<void *>(context.Rip));

        DWORD64           image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void   *handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                         &establisher, nullptr);
    }
    std::fprintf(out, "==== end of crash report ====\n");
    std::fflush(out);
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS *info) {
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    Report(stderr, info);

    wchar_t path[MAX_PATH];
    DWORD   length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/') {
        --length;
    }
    if (length > 0 && length + 20 < MAX_PATH) {
        std::wcscpy(path + length, L"crash_report.txt");
        if (FILE *file = _wfopen(path, L"w")) {
            Report(file, info);
            std::fclose(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// First chance, before any frame's handler can swallow or redirect it: an access violation is reported
// here even when the unhandled filter never runs. Handled ones are rare, so the first few are kept.
LONG WINAPI OnFirstChance(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const LONG count = InterlockedIncrement(&g_reports);
    if (count > 4) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::fprintf(stderr, "(first chance %ld)", static_cast<long>(count));
    Report(stderr, info);

    wchar_t path[MAX_PATH];
    DWORD   length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/') {
        --length;
    }
    if (length > 0 && length + 20 < MAX_PATH) {
        std::wcscpy(path + length, L"crash_report.txt");
        if (FILE *file = _wfopen(path, count == 1 ? L"w" : L"a")) {
            std::fprintf(file, "(first chance %ld)", static_cast<long>(count));
            Report(file, info);
            std::fclose(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void CrashReportNote(const char *text) {
    std::snprintf(g_note, sizeof g_note, "%s", text != nullptr ? text : "");
}

void CrashReportInstall() {
    AddVectoredExceptionHandler(1, OnFirstChance);
    SetUnhandledExceptionFilter(OnUnhandled);
    // The last lines before a crash reach crash.out.log too.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stderr, "crash report: main thread %lu\n", static_cast<unsigned long>(GetCurrentThreadId()));
}

#endif
