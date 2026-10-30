#include "headpose.h"
#include <windows.h>
#include <mutex>
#include <cmath>

namespace mea {

static std::mutex g_mx;
static HeadState g_head;
static RenderedView g_rendered;

void HeadPublish(const HeadState& s) { std::lock_guard<std::mutex> l(g_mx); g_head = s; }
HeadState HeadRead() { std::lock_guard<std::mutex> l(g_mx); return g_head; }
void RenderedPublish(const RenderedView& r) { std::lock_guard<std::mutex> l(g_mx); g_rendered = r; }
RenderedView RenderedRead() { std::lock_guard<std::mutex> l(g_mx); return g_rendered; }

void QuatMul(const float* a, const float* b, float* o) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}

void QuatRotate(const float* q, const float* v, float* o) {
    // v' = v + 2w (q x v) + 2 q x (q x v)
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float tx = 2 * (qy * v[2] - qz * v[1]), ty = 2 * (qz * v[0] - qx * v[2]), tz = 2 * (qx * v[1] - qy * v[0]);
    o[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    o[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    o[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

Pose PoseMul(const Pose& a, const Pose& b) {
    Pose r;
    QuatMul(a.q, b.q, r.q);
    float t[3];
    QuatRotate(a.q, b.p, t);
    for (int i = 0; i < 3; ++i) r.p[i] = a.p[i] + t[i];
    return r;
}

void QuatFromYaw(float yaw, float* q) { q[0] = 0; q[1] = std::sin(yaw * 0.5f); q[2] = 0; q[3] = std::cos(yaw * 0.5f); }

}
