// Head aim, step 1: find the game's camera yaw / pitch control values in memory (the values the right stick drives).
// The game's own camera (EngineGameCamera, before our head pose) gives the true yaw and pitch each frame. A scan
// collects every float in writable memory that equals the yaw or the pitch under one of a few conventions
// (radians or degrees, sign, quarter-turn offset); later scans (after turning the camera) keep only the candidates
// still consistent with their convention. aimtest then writes a small change into each survivor and keeps the ones
// that actually turn the camera: those are the control values head aim will write.
//   MEAVR\aimscan.trigger  - first scan, or refine (camera must be still; turn between scans)
//   MEAVR\aimreset.trigger - forget the candidates
//   MEAVR\aimtest.trigger  - write test of the survivors (at most 64)
// Results: log + MEAVR\aimscan.txt.
#include "headaim.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include <string>
#include <cmath>
#include <cstdio>
#include "aimscan.h"
#include "engine.h"
#include "globals.h"
#include "log.h"

namespace mea {

static const float kPi = 3.14159265f;
// hypothesis: kind (0 yaw, 1 pitch), sign, offset (quarter turns), unit (0 rad, 1 deg)
struct Hyp { int kind, sign, quarter, deg; };
static std::vector<Hyp> g_hyps;
static std::vector<uint64_t> g_cand;     // address | hyp << 56
static int g_scans = 0;
static volatile LONG g_busy = 0;
static float g_lastYaw = 0, g_lastPitch = 0;

static void BuildHyps() {
    if (!g_hyps.empty()) return;
    for (int deg = 0; deg < 2; ++deg)
        for (int s = -1; s <= 1; s += 2) {
            for (int q = 0; q < 4; ++q) g_hyps.push_back({0, s, q, deg});
            for (int q = 0; q < 2; ++q) g_hyps.push_back({1, s, q, deg});
        }
}

static bool YawPitch(float& yaw, float& pitch) {
    float c[16];
    if (!EngineGameCamera(c)) return false;
    float f[3] = {-c[8], -c[9], -c[10]};   // forward = -back
    yaw = std::atan2(f[0], f[2]);
    pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, f[1])));
    return true;
}

// the value hypothesis h predicts (rad or deg), period 2 pi / 360 for yaw
static float Predict(const Hyp& h, float yaw, float pitch) {
    float a = h.kind == 0 ? h.sign * yaw + h.quarter * kPi * 0.5f : h.sign * pitch + h.quarter * kPi * 0.5f;
    return h.deg ? a * 57.2957795f : a;
}
static bool Match(const Hyp& h, float v, float pred) {
    if (!(std::fabs(v) < 800.f)) return false;   // also NaN; huge values would wrap to anything
    float tol = h.deg ? 0.2f : 0.0035f;
    float d = v - pred;
    if (h.kind == 0) {   // yaw wraps
        float per = h.deg ? 360.f : 2.f * kPi;
        d = d - per * std::round(d / per);
    }
    return std::fabs(d) <= tol;
}

static bool Writable(const MEMORY_BASIC_INFORMATION& m) {
    DWORD pr = m.Protect & 0xFF;
    return m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE);
}

static DWORD WINAPI ScanThread(void*) {
    float yaw, pitch;
    if (!YawPitch(yaw, pitch)) { LOGW("aimscan: no game camera yet (load into the game first)"); InterlockedExchange(&g_busy, 0); return 0; }
    BuildHyps();
    HANDLE self = GetCurrentProcess();
    DWORD t0 = GetTickCount();
    size_t before = g_cand.size();
    if (g_scans == 0) {
        // targets: every (hyp, wrap) value, sorted, for a quick lookup per float
        struct T { float v; int h; };
        std::vector<T> tg;
        for (int i = 0; i < (int)g_hyps.size(); ++i) {
            float p = Predict(g_hyps[i], yaw, pitch);
            float per = g_hyps[i].deg ? 360.f : 2.f * kPi;
            int kmax = g_hyps[i].kind == 0 ? 2 : 0;
            for (int k = -kmax; k <= kmax; ++k) tg.push_back({p + k * per, i});
        }
        std::sort(tg.begin(), tg.end(), [](const T& a, const T& b) { return a.v < b.v; });
        std::vector<uint8_t> buf(1 << 20);
        MEMORY_BASIC_INFORMATION m{};
        size_t bytes = 0;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull; a = (uintptr_t)m.BaseAddress + m.RegionSize) {
            if (!VirtualQuery((void*)a, &m, sizeof(m))) break;
            if (!Writable(m)) continue;
            for (uintptr_t c = (uintptr_t)m.BaseAddress; c < (uintptr_t)m.BaseAddress + m.RegionSize; c += buf.size()) {
                size_t n = std::min(buf.size(), (size_t)((uintptr_t)m.BaseAddress + m.RegionSize - c));
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, (void*)c, buf.data(), n, &got) || got < 4) break;
                bytes += got;
                const float* f = (const float*)buf.data();
                for (size_t i = 0; i < got / 4; ++i) {
                    float v = f[i];
                    if (!(std::fabs(v) < 800.f) || v == 0.f) continue;
                    auto it = std::lower_bound(tg.begin(), tg.end(), v - 0.2f, [](const T& t, float x) { return t.v < x; });
                    for (; it != tg.end() && it->v <= v + 0.2f; ++it)
                        if (Match(g_hyps[it->h], v, it->v)) { g_cand.push_back((uint64_t)(c + i * 4) | ((uint64_t)it->h << 56)); break; }
                    if (g_cand.size() > 60000000) break;
                }
            }
        }
        LOGI("aimscan: first scan (yaw %.3f, pitch %.3f): %zu MB read, %zu candidates, %lu ms", yaw, pitch, bytes >> 20, g_cand.size(), GetTickCount() - t0);
        if (std::fabs(yaw) < 0.2f) LOGW("aimscan: yaw is close to 0 - many false hits; turn the camera before the first scan next time");
    } else {
        std::vector<uint64_t> keep;
        keep.reserve(g_cand.size() / 4);
        for (uint64_t e : g_cand) {
            uintptr_t addr = (uintptr_t)(e & 0x00FFFFFFFFFFFFFFull);
            const Hyp& h = g_hyps[e >> 56];
            float v; SIZE_T got = 0;
            if (!ReadProcessMemory(self, (void*)addr, &v, 4, &got) || got != 4) continue;
            if (Match(h, v, Predict(h, yaw, pitch))) keep.push_back(e);
        }
        g_cand.swap(keep);
        LOGI("aimscan: refine %d (yaw %.3f [%+.3f], pitch %.3f [%+.3f]): %zu -> %zu candidates, %lu ms", g_scans, yaw, yaw - g_lastYaw, pitch,
             pitch - g_lastPitch, before, g_cand.size(), GetTickCount() - t0);
    }
    g_lastYaw = yaw; g_lastPitch = pitch;
    ++g_scans;
    // summary per hypothesis + the first addresses
    std::wstring path = G().outDir + L"aimscan.txt";
    if (FILE* fo = _wfopen(path.c_str(), L"w")) {
        fprintf(fo, "scan %d: yaw %.4f pitch %.4f, %zu candidates\n", g_scans, yaw, pitch, g_cand.size());
        if (g_cand.size() <= 2000)
            for (uint64_t e : g_cand) {
                const Hyp& h = g_hyps[e >> 56];
                uintptr_t addr = (uintptr_t)(e & 0x00FFFFFFFFFFFFFFull);
                float v = 0; SIZE_T got; ReadProcessMemory(self, (void*)addr, &v, 4, &got);
                fprintf(fo, "%p %s sign %+d q%d %s value %.4f\n", (void*)addr, h.kind ? "pitch" : "yaw", h.sign, h.quarter, h.deg ? "deg" : "rad", v);
            }
        fclose(fo);
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

static DWORD WINAPI TestThread(void*) {
    HANDLE self = GetCurrentProcess();
    std::wstring path = G().outDir + L"aimscan.txt";
    FILE* fo = _wfopen(path.c_str(), L"a");
    int tested = 0, good = 0;
    std::vector<uint64_t> order;   // yaw candidates first
    for (int pass = 0; pass < 2; ++pass)
        for (uint64_t e : g_cand) if (g_hyps[e >> 56].kind == pass) order.push_back(e);
    for (size_t i = 0; i < order.size() && tested < 300; ++i) {
        uint64_t e = order[i];
        uintptr_t addr = (uintptr_t)(e & 0x00FFFFFFFFFFFFFFull);
        const Hyp& h = g_hyps[e >> 56];
        float y0, p0, y1, p1, v;
        SIZE_T got;
        if (!YawPitch(y0, p0) || !ReadProcessMemory(self, (void*)addr, &v, 4, &got)) continue;
        ++tested;
        float delta = 0.15f;   // rad of camera turn wanted
        float w = v + h.sign * (h.deg ? delta * 57.2957795f : delta);
        SIZE_T put;
        WriteProcessMemory(self, (void*)addr, &w, 4, &put);
        Sleep(200);
        YawPitch(y1, p1);
        float dy = y1 - y0; dy -= 2.f * kPi * std::round(dy / (2.f * kPi));
        float dp = p1 - p0;
        float moved = h.kind == 0 ? dy : dp;
        bool hit = std::fabs(moved - delta) < 0.05f;
        if (!hit) {   // no (or wrong) effect: restore
            float now; ReadProcessMemory(self, (void*)addr, &now, 4, &got);
            if (now == w) WriteProcessMemory(self, (void*)addr, &v, 4, &put);
        } else {
            ++good;
            static bool have[2] = {false, false};
            if (!have[h.kind]) { have[h.kind] = true; HeadAimSetTarget(h.kind, addr, h.sign, h.deg != 0); }
        }
        LOGI("aimtest: %p %s sign %+d q%d %s: camera %s %+.3f (wanted %+.3f)%s", (void*)addr, h.kind ? "pitch" : "yaw", h.sign, h.quarter, h.deg ? "deg" : "rad",
             h.kind ? "pitch" : "yaw", moved, delta, hit ? "  <== CONTROLS THE CAMERA" : "");
        if (fo) fprintf(fo, "test %p %s sign %+d q%d %s moved %+.3f%s\n", (void*)addr, h.kind ? "pitch" : "yaw", h.sign, h.quarter, h.deg ? "deg" : "rad", moved,
                        hit ? "  CONTROL" : "");
        Sleep(60);
    }
    LOGI("aimtest: %d tested, %d control the camera", tested, good);
    if (fo) fclose(fo);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

void AimScanPoll() {
    auto take = [](const wchar_t* name) {
        std::wstring t = G().outDir + name;
        if (GetFileAttributesW(t.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        DeleteFileW(t.c_str());
        return true;
    };
    if (take(L"aimreset.trigger")) {
        if (g_busy) LOGW("aimscan: busy");
        else { g_cand.clear(); g_cand.shrink_to_fit(); g_scans = 0; LOGI("aimscan: reset"); }
    }
    if (take(L"aimscan.trigger")) {
        if (InterlockedCompareExchange(&g_busy, 1, 0) == 0) CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
        else LOGW("aimscan: busy");
    }
    if (take(L"aimtest.trigger")) {
        if (g_cand.size() > 5000) LOGW("aimtest: %zu candidates - refine first", g_cand.size());
        else if (InterlockedCompareExchange(&g_busy, 1, 0) == 0) CreateThread(nullptr, 0, TestThread, nullptr, 0, nullptr);
        else LOGW("aimscan: busy");
    }
}

}
