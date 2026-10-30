// Camera finder. Step 1: catch Frostbite's per-view constant block while the game writes it into the constant ring
// (signature: a row (W, H, 1/W, 1/H) followed 8 rows later by the projection row (0, 0, -1, 0)). Step 2 (worker thread):
// search the process's writable memory for the CPU-side camera: the camera world transform (right / up / forward rows,
// 16- or 12-byte stride), the view matrix rows, and the camera position. Hits go to MEAVR\camscan_NNN.txt with the
// surrounding floats, so the game's camera object and the code that writes it can be found next.
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include "camscan.h"
#include "globals.h"
#include "log.h"

namespace mea {

static volatile LONG g_armed = 0;      // 1: look for the view block in the next buffer uploads
static volatile LONG g_have = 0;       // 1: block captured, scan pending
static float g_view[80 * 4];           // the 1280-byte block (80 float4 rows) from row "screen" - 1
static UINT g_w = 0, g_h = 0;
static int g_scanIndex = 0;
static volatile LONG g_scanning = 0;
static bool g_autoWatch = false;
static int g_autoWatchSeconds = 3;
void CamScanSetAutoWatch(int seconds) { g_autoWatch = true; g_autoWatchSeconds = seconds; }

void CamScanRequest(UINT w, UINT h) {
    if (g_scanning) { LOGW("camscan: a scan is still running"); return; }
    g_w = w; g_h = h;
    InterlockedExchange(&g_have, 0);
    InterlockedExchange(&g_armed, 1);
    LOGI("camscan: armed (render size %ux%u)", w, h);
}
bool CamScanArmed() { return g_armed != 0; }

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Called from the Unmap hook (before the real Unmap) for every buffer upload while armed.
void CamScanInspect(const void* data, size_t bytes) {
    if (!g_armed || !data || bytes < 1280) return;
    const float* f = (const float*)data;
    size_t rows = bytes / 16;
    float W = (float)g_w, H = (float)g_h;
    for (size_t r = 1; r + 40 < rows; ++r) {
        const float* q = f + r * 4;
        if (q[0] != W || q[1] != H) continue;
        if (!Near(q[2], 1.f / W, 1e-6f) || !Near(q[3], 1.f / H, 1e-6f)) continue;
        const float* p0 = f + (r + 5) * 4;                   // projection rows 0, 1, 3
        const float* p1 = f + (r + 6) * 4;
        const float* p3 = f + (r + 8) * 4;
        if (!(p3[0] == 0 && p3[1] == 0 && Near(p3[2], -1.f, 1e-5f) && p3[3] == 0)) continue;
        if (!(p0[1] == 0 && p0[2] == 0 && p0[3] == 0 && p1[0] == 0 && p1[2] == 0 && p1[3] == 0)) continue;
        if (!(p0[0] > 0.05f && p1[1] > 0.05f && Near(p1[1] / p0[0], W / H, 0.01f))) continue;   // the screen view, not a square shadow / probe view
        const float* rr = f + (r + 1) * 4;                   // view rotation row 0 must be a unit vector
        if (!Near(rr[0] * rr[0] + rr[1] * rr[1] + rr[2] * rr[2], 1.f, 1e-3f)) continue;
        size_t start = r - 1, n = rows - start < 80 ? rows - start : 80;
        memset(g_view, 0, sizeof(g_view));
        memcpy(g_view, f + start * 4, n * 16);
        InterlockedExchange(&g_armed, 0);
        InterlockedExchange(&g_have, 1);
        LOGI("camscan: view block found at ring row %zu", start);
        return;
    }
}

struct Hit { uintptr_t addr; int kind; int stride; };

// MSVC RTTI: class name of a vtable in the game exe ("" if p is not a vtable). vtable[-1] -> CompleteObjectLocator
// (signature 1 on x64, then RVAs relative to the image base).
static uintptr_t g_exeBase = 0, g_exeEnd = 0;
static bool InExe(uintptr_t p) { return p >= g_exeBase && p < g_exeEnd; }
static std::string VtableClass(uintptr_t vt) {
    if (!InExe(vt) || (vt & 7)) return "";
    uintptr_t col = 0; SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)(vt - 8), &col, 8, &got) || !InExe(col)) return "";
    uint32_t c[6] = {};
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)col, c, sizeof(c), &got) || c[0] != 1) return "";
    uintptr_t td = g_exeBase + c[3];
    char name[160] = {};
    if (!InExe(td) || !ReadProcessMemory(GetCurrentProcess(), (void*)(td + 16), name, sizeof(name) - 1, &got)) return "";
    if (name[0] != '.' || name[1] != '?') return "";
    return name;
}
// Owner objects of an address: vtable pointers found in the 0x1000 bytes before it (nearest first).
static void PrintOwners(FILE* f, uintptr_t a) {
    if (!g_exeBase) {
        auto base = (uint8_t*)GetModuleHandleW(nullptr);
        auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        g_exeBase = (uintptr_t)base; g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
    }
    uintptr_t q[512]; SIZE_T got = 0;
    uintptr_t from = (a & ~(uintptr_t)7) - sizeof(q);
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)from, q, sizeof(q), &got) || got != sizeof(q)) return;
    int shown = 0;
    for (int i = 511; i >= 0 && shown < 4; --i) {
        std::string n = VtableClass(q[i]);
        if (n.empty()) continue;
        uintptr_t obj = from + i * 8;
        fprintf(f, "  owner? %s at %p (hit is +0x%llX), vtable exe+%llX\n", n.c_str(), (void*)obj, (unsigned long long)(a - obj),
                (unsigned long long)(q[i] - g_exeBase));
        ++shown;
    }
}

static std::string RegionName(uintptr_t a) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* b = strrchr(name, '\\');
        char out[MAX_PATH + 32];
        snprintf(out, sizeof(out), "%s+0x%llX", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
        return out;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery((void*)a, &mbi, sizeof(mbi));
    char out[96];
    snprintf(out, sizeof(out), "heap (alloc base %p, region %p +0x%llX)", mbi.AllocationBase, mbi.BaseAddress,
             (unsigned long long)(a - (uintptr_t)mbi.BaseAddress));
    return out;
}

static DWORD WINAPI ScanThread(void*) {
    // camera axes from the view matrix rows (view = [R | -R*pos]; rows of R = camera right, up, back in world space)
    const float* V = g_view;
    const float* rRight = V + 2 * 4; const float* rUp = V + 3 * 4; const float* rBack = V + 4 * 4;
    const float* pos = V + 37 * 4;
    float fwd[3] = {-rBack[0], -rBack[1], -rBack[2]};
    float cotHalf = V[7 * 4 + 1];
    float fovY = 2.f * std::atan(1.f / cotHalf);
    const float tol = 2e-3f, ptol = 0.05f;

    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0);
    std::vector<Hit> hits;
    std::vector<float> buf(1 << 20);       // 4 MB chunks
    size_t scanned = 0;
    HANDLE self = GetCurrentProcess();
    uintptr_t a = 0x10000;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t end = (uintptr_t)si.lpMaximumApplicationAddress;
    while (a < end && hits.size() < 4000) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        uintptr_t rb = (uintptr_t)mbi.BaseAddress, re = rb + mbi.RegionSize;
        DWORD pr = mbi.Protect & 0xFF;
        bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                  (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_WRITECOPY);
        if (rw && rb != (uintptr_t)buf.data()) {
            for (uintptr_t c = rb; c < re; c += buf.size() * 4 - 64) {
                uintptr_t cap = (uintptr_t)(buf.size() * 4); size_t n = (size_t)(re - c < cap ? re - c : cap);
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, (void*)c, buf.data(), n, &got) || got < 64) break;
                size_t cnt = got / 4;
                scanned += got;
                const float* b = buf.data();
                for (size_t i = 0; i + 16 < cnt; ++i) {
                    float x = b[i];
                    // camera right vector (start of a world transform or of the view matrix rows)
                    if (Near(x, rRight[0], tol) && Near(b[i + 1], rRight[1], tol) && Near(b[i + 2], rRight[2], tol)) {
                        for (int stride : {4, 3}) {
                            const float* u = b + i + stride;
                            if (Near(u[0], rUp[0], tol) && Near(u[1], rUp[1], tol) && Near(u[2], rUp[2], tol)) {
                                const float* z = b + i + 2 * stride;
                                int kind = (Near(z[0], fwd[0], tol) && Near(z[1], fwd[1], tol) && Near(z[2], fwd[2], tol)) ? 1
                                         : (Near(z[0], rBack[0], tol) && Near(z[1], rBack[1], tol) && Near(z[2], rBack[2], tol)) ? 2 : 3;
                                hits.push_back({c + i * 4, kind, stride});
                            }
                        }
                    }
                    // camera right as a matrix COLUMN (column-major / transposed storage)
                    if (Near(x, rRight[0], tol) && Near(b[i + 4], rRight[1], tol) && Near(b[i + 8], rRight[2], tol) &&
                        Near(b[i + 1], rUp[0], tol) && Near(b[i + 5], rUp[1], tol))
                        hits.push_back({c + i * 4, 4, 4});
                    // camera position
                    if (Near(x, pos[0], ptol) && Near(b[i + 1], pos[1], ptol) && Near(b[i + 2], pos[2], ptol))
                        hits.push_back({c + i * 4, 5, 0});
                }
                if (got < n) break;
            }
        }
        a = re;
    }
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f0.QuadPart;

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%scamscan_%03d.txt", G().outDir.c_str(), g_scanIndex++);
    FILE* f = _wfopen(path, L"w");
    if (f) {
        fprintf(f, "# camera from the view constants: right %.4f %.4f %.4f  up %.4f %.4f %.4f  back %.4f %.4f %.4f\n", rRight[0], rRight[1],
                rRight[2], rUp[0], rUp[1], rUp[2], rBack[0], rBack[1], rBack[2]);
        fprintf(f, "# position %.3f %.3f %.3f  fovY %.4f rad (%.2f deg)  near %.4f\n", pos[0], pos[1], pos[2], fovY, fovY * 57.29578f, V[8 * 4 + 3]);
        fprintf(f, "# scanned %.0f MB in %.0f ms, %zu hits\n", scanned / 1048576.0, ms, hits.size());
        fprintf(f, "# kinds: 1 transform right/up/forward  2 right/up/back (view rows)  3 right/up + other  4 column-major  5 position\n");
        fprintf(f, "# view block rows:\n");
        for (int r = 0; r < 40; ++r) fprintf(f, "#  [%2d] %12.5f %12.5f %12.5f %12.5f\n", r, V[r * 4], V[r * 4 + 1], V[r * 4 + 2], V[r * 4 + 3]);
        for (auto& h : hits) {
            fprintf(f, "\nHIT kind %d stride %d at %p  %s\n", h.kind, h.stride * 4, (void*)h.addr, RegionName(h.addr).c_str());
            if (h.kind != 5) PrintOwners(f, h.addr);
            if (h.kind == 5 && hits.size() > 600) continue;   // many position hits: no dumps
            float around[48]; SIZE_T got = 0;
            if (ReadProcessMemory(self, (void*)(h.addr - 64), around, sizeof(around), &got) && got == sizeof(around))
                for (int r = 0; r < 12; ++r)
                    fprintf(f, "  %+5d: %12.5f %12.5f %12.5f %12.5f\n", (r * 16) - 64, around[r * 4], around[r * 4 + 1], around[r * 4 + 2], around[r * 4 + 3]);
        }
        fclose(f);
    }
    int counts[6] = {};
    for (auto& h : hits) counts[h.kind]++;
    if (g_autoWatch) {
        // watch one representative of each kind (different allocations): which game code writes the camera copies
        auto regionOf = [](uintptr_t a) { MEMORY_BASIC_INFORMATION m{}; VirtualQuery((void*)a, &m, sizeof(m)); return (uintptr_t)m.AllocationBase; };
        std::vector<std::pair<uintptr_t, const char*>> w;
        const Hit* k1 = nullptr;
        for (auto& h : hits) if (h.kind == 1 && h.stride == 4) { k1 = &h; break; }
        if (k1) w.push_back({k1->addr + 48, "transform.pos.x"});
        uintptr_t r1 = k1 ? regionOf(k1->addr) : 0;

        // column-major view matrices (Frostbite RenderView layout): row 3 = (tx, ty, tz, 1) with |t| = |R * pos|
        {
            float tv[3] = {V[2 * 4 + 3], V[3 * 4 + 3], V[4 * 4 + 3]};       // view translation from the constants
            float tlen = std::sqrt(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
            std::vector<uintptr_t> regionsUsed;
            for (auto& h : hits) {
                if (h.kind != 4 || w.size() >= 4) continue;
                float row[4]; SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (void*)(h.addr + 48), row, 16, &got)) continue;
                float l = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                if (!Near(row[3], 1.f, 1e-4f) || !Near(l, tlen, tlen * 0.02f + 0.05f)) continue;
                uintptr_t rg = regionOf(h.addr);
                bool dup = false; for (auto r : regionsUsed) dup |= r == rg;
                if (dup) continue;
                regionsUsed.push_back(rg);
                w.push_back({h.addr + 48, regionsUsed.size() == 1 ? "renderview.trans.x" : "renderview2.trans.x"});
            }
        }
        std::wstring t = G().outDir + L"watch.trigger";
        if (FILE* wf = _wfopen(t.c_str(), L"w")) {
            fprintf(wf, "seconds = %d\n", g_autoWatchSeconds);
            for (size_t i = 0; i < w.size() && i < 4; ++i) fprintf(wf, "%llx %s\n", (unsigned long long)w[i].first, w[i].second);
            fclose(wf);
        }
        g_autoWatch = false;
        LOGI("camscan: watch armed on %zu addresses", w.size() < 4 ? w.size() : (size_t)4);
    }
    LOGI("camscan: %S - %.0f MB in %.0f ms: %d transform, %d view rows, %d right/up, %d column-major, %d position hits", path,
         scanned / 1048576.0, ms, counts[1], counts[2], counts[3], counts[4], counts[5]);
    InterlockedExchange(&g_scanning, 0);
    return 0;
}

void CamScanOnPresent() {
    if (g_have && !g_scanning) {
        InterlockedExchange(&g_have, 0);
        InterlockedExchange(&g_scanning, 1);
        CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    }
}

} // namespace mea
