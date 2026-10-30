// Logs fatal-looking exceptions (access violation, illegal instruction, ...) with module+RVA and a short
// unwound stack. Never handles anything: always continues the search, so the game's own handlers run.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include "crash.h"
#include "log.h"

namespace mea {

static std::atomic<int> g_count{0};

static void Where(DWORD64 addr, char* out, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) && m) {
        char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
        const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
        snprintf(out, n, "%s+%llX", b, (unsigned long long)(addr - (DWORD64)m));
    } else snprintf(out, n, "%016llX", (unsigned long long)addr);
}

static bool Fatal(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_IN_PAGE_ERROR:
    case 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN / fail fast */: case 0xC0000374 /* heap corruption */:
        return true;
    }
    return false;
}

static LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!Fatal(code)) return EXCEPTION_CONTINUE_SEARCH;
    int n = g_count.fetch_add(1);
    if (n >= 20) return EXCEPTION_CONTINUE_SEARCH;
    char w[200]; Where((DWORD64)ep->ExceptionRecord->ExceptionAddress, w, sizeof(w));
    ULONG_PTR a0 = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    ULONG_PTR a1 = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    LOGE("exception %08lX at %s (thread %lu) info %llX %llX  rax=%llX rcx=%llX rdx=%llX r8=%llX rsp=%llX",
         code, w, GetCurrentThreadId(), (unsigned long long)a0, (unsigned long long)a1,
         ep->ContextRecord->Rax, ep->ContextRecord->Rcx, ep->ContextRecord->Rdx, ep->ContextRecord->R8, ep->ContextRecord->Rsp);
    if (code == EXCEPTION_STACK_OVERFLOW) { LogFlush(); return EXCEPTION_CONTINUE_SEARCH; }
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 16 && c.Rip; i++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!f) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }   // leaf
        else { void* hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &ef, nullptr); }
        if (!c.Rip) break;
        Where(c.Rip, w, sizeof(w));
        LOGE("    #%d %s", i, w);
    }
    LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void CrashInstall() {
    AddVectoredExceptionHandler(1, Handler);
    LOGI("crash logger installed");
}

} // namespace mea
