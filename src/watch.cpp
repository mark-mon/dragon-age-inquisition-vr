// Hardware write watchpoints (debug registers DR0-DR3) for reverse engineering, x64: logs which code writes the
// given addresses, with return addresses into the game exe found on the stack.
// Automation: write MEAVR\watch.trigger with up to 4 lines "<hex address> [label]" (optionally "seconds = N");
// the result is MEAVR\watch_NNN.txt.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <vector>
#include "watch.h"
#include "globals.h"
#include "log.h"

namespace mea {

static uintptr_t g_addr[4];
static bool g_read[4];
static std::string g_label[4];
static int g_n = 0;
static PVOID g_veh = nullptr;
static std::mutex g_mx;
struct Hit { int count; uint32_t stack[10]; int nstack; DWORD tid; float value[4]; };
static std::map<std::pair<int, uint64_t>, Hit> g_hits;   // (watch index, rip) -> hit
static volatile LONG g_active = 0;
static DWORD g_stopAt = 0;
static int g_index = 0;
static uintptr_t g_exeBase = 0, g_textBase = 0, g_textEnd = 0;

static void InitExeRange() {
    if (g_exeBase) return;
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    g_exeBase = (uintptr_t)base;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (memcmp(sec->Name, ".text", 5) == 0) { g_textBase = g_exeBase + sec->VirtualAddress; g_textEnd = g_textBase + sec->Misc.VirtualSize; }
}

static bool LooksLikeReturn(uintptr_t a) {
    if (a < g_textBase + 8 || a >= g_textEnd) return false;
    const uint8_t* p = (const uint8_t*)a;
    if (p[-5] == 0xE8) return true;                                  // call rel32
    if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return true;       // call r/m (short forms)
    if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return true;
    if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return true;       // call [rip+disp32]
    if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2) return true;
    return false;
}

static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_active) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < g_n; ++i) {
        if (!(dr6 & (1ull << i))) continue;
        std::lock_guard<std::mutex> lk(g_mx);
        auto& h = g_hits[{i, (uint64_t)c->Rip}];
        if (h.count++ == 0) {
            h.tid = GetCurrentThreadId();
            h.nstack = 0;
            const uintptr_t* sp = (const uintptr_t*)c->Rsp;
            NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
            for (int k = 0; k < 512 && (uintptr_t)(sp + k) < (uintptr_t)tib->StackBase && h.nstack < 10; ++k)
                if (LooksLikeReturn(sp[k])) h.stack[h.nstack++] = (uint32_t)(sp[k] - g_exeBase);
            memcpy(h.value, (const void*)g_addr[i], sizeof(h.value));
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ApplyAll(bool set) {
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        bool other = te.th32ThreadID != self;
        if (other) SuspendThread(th);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &ctx)) {
            DWORD64 dr7 = 0;
            ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
            if (set)
                for (int i = 0; i < g_n; ++i) {
                    (&ctx.Dr0)[i] = g_addr[i];
                    dr7 |= 1ull << (i * 2);            // local enable
                    dr7 |= (g_read[i] ? 3ull : 1ull) << (16 + i * 4);   // RW = 01 writes, 11 reads/writes
                    dr7 |= 3ull << (18 + i * 4);       // LEN = 11: 4 bytes
                }
            ctx.Dr7 = dr7;
            ctx.Dr6 = 0;
            if (SetThreadContext(th, &ctx)) ++n;
        }
        if (other) ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    LOGI("watch: %s debug registers on %d threads", set ? "set" : "cleared", n);
}

static void Start(const std::vector<std::pair<uintptr_t, std::string>>& w, int seconds) {
    if (g_active || w.empty()) return;
    InitExeRange();
    g_n = (int)(w.size() > 4 ? 4 : w.size());
    for (int i = 0; i < g_n; ++i) { g_addr[i] = w[i].first & ~(uintptr_t)3; g_label[i] = w[i].second; g_read[i] = g_label[i].rfind("r:", 0) == 0; }
    { std::lock_guard<std::mutex> lk(g_mx); g_hits.clear(); }
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    InterlockedExchange(&g_active, 1);
    ApplyAll(true);
    g_stopAt = GetTickCount() + seconds * 1000;
    for (int i = 0; i < g_n; ++i) LOGI("watch: [%d] %p %s", i, (void*)g_addr[i], g_label[i].c_str());
}

// start of the function containing an address (x64 unwind tables), as exe RVA; 0 if unknown
static uint32_t FuncStart(uint64_t a) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(a, &base, nullptr);
    return (rf && base == g_exeBase) ? rf->BeginAddress : 0;
}

static void Stop() {
    ApplyAll(false);
    InterlockedExchange(&g_active, 0);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%swatch_%03d.txt", G().outDir.c_str(), g_index++);
    FILE* f = _wfopen(path, L"w");
    std::lock_guard<std::mutex> lk(g_mx);
    if (f) {
        fprintf(f, "# exe base %p  .text %p-%p\n", (void*)g_exeBase, (void*)g_textBase, (void*)g_textEnd);
        for (int i = 0; i < g_n; ++i) fprintf(f, "# [%d] %p %s\n", i, (void*)g_addr[i], g_label[i].c_str());
        for (auto& kv : g_hits) {
            const Hit& h = kv.second;
            uint64_t rip = kv.first.second;
            bool inExe = rip >= g_exeBase && rip < g_textEnd;
            fprintf(f, "[%d %s] rip=%s%llX hits=%d tid=%lu value %.4f %.4f %.4f %.4f stack:", kv.first.first, g_label[kv.first.first].c_str(),
                    inExe ? "exe+" : "", (unsigned long long)(inExe ? rip - g_exeBase : rip), h.count, h.tid, h.value[0], h.value[1], h.value[2], h.value[3]);
            for (int i = 0; i < h.nstack; ++i) fprintf(f, " exe+%X", h.stack[i]);
            fprintf(f, "\n      function exe+%X; callers:", inExe ? FuncStart(rip) : 0);
            for (int i = 0; i < h.nstack; ++i) fprintf(f, " exe+%X", FuncStart(g_exeBase + h.stack[i]));
            fprintf(f, "\n");
        }
        fclose(f);
    }
    LOGI("watch: %S - %zu distinct writers", path, g_hits.size());
}

// Writes the game exe's in-memory image (decrypted by the DRM wrapper at start-up) to MEAVR\exe_image.bin, so its code
// can be disassembled offline. Unreadable pages are zero-filled.
static void DumpExeImage() {
    InitExeRange();
    auto base = (uint8_t*)g_exeBase;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;
    std::wstring path = G().outDir + L"exe_image.bin";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    std::vector<uint8_t> page(1 << 16);
    size_t bad = 0;
    for (size_t off = 0; off < size; off += page.size()) {
        size_t n = size - off < page.size() ? size - off : page.size();
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), base + off, page.data(), n, &got) || got != n) {
            // fall back page by page
            for (size_t p = 0; p < n; p += 4096) {
                SIZE_T g2 = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), base + off + p, page.data() + p, 4096, &g2)) { memset(page.data() + p, 0, 4096); ++bad; }
            }
        }
        fwrite(page.data(), 1, n, f);
    }
    fclose(f);
    LOGI("exe image: %S, %zu MB (%zu unreadable pages), base %p", path.c_str(), size >> 20, bad, base);
}

void WatchOnPresent(uint64_t presentIndex) {
    if ((presentIndex % 30) == 15) {
        std::wstring d = G().outDir + L"dumpexe.trigger";
        if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(d.c_str()); DumpExeImage(); }
    }
    if (g_active && (LONG)(GetTickCount() - g_stopAt) >= 0) Stop();
    if (g_active || (presentIndex % 30) != 0) return;
    std::wstring t = G().outDir + L"watch.trigger";
    FILE* f = _wfopen(t.c_str(), L"r");
    if (!f) return;
    std::vector<std::pair<uintptr_t, std::string>> w;
    int seconds = 3;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long a = 0; char label[128] = "";
        if (sscanf(line, " seconds = %d", &seconds) == 1) continue;
        if (sscanf(line, " %llx %127s", &a, label) >= 1 && a) w.push_back({(uintptr_t)a, label});
    }
    fclose(f);
    DeleteFileW(t.c_str());
    Start(w, seconds);
}

} // namespace mea
