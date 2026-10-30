#pragma once
// Head pose shared between the OpenXR frame loop (writer, render thread) and the engine RenderView hook (readers,
// any game thread). Poses are relative to the recentre reference: x right, y up, -z forward (OpenXR convention).
#include <cstdint>
namespace mea {

struct Pose { float q[4] = {0, 0, 0, 1}; float p[3] = {0, 0, 0}; };   // q = x, y, z, w

struct HeadState {
    bool valid = false;
    Pose head;          // head (centre eye) relative to the reference
    Pose eye[2];        // eye poses relative to the head (from xrLocateViews)
    float fov[2][4] = {};   // per-eye fov angles: left, right, up, down (radians)
    uint64_t frame = 0; // xr frame counter when written
};

void HeadPublish(const HeadState& s);
HeadState HeadRead();

// What the engine actually rendered with (written by the RenderView hook, read at Present for layer submission).
struct RenderedView {
    bool valid = false;
    Pose head;          // the head pose applied to the main view
    float tanX = 1, tanY = 1;   // symmetric half-angle tangents of the main view
    uint64_t frame = 0;
    // stereo: per-eye pose (relative to the reference) and tangents
    bool stereo = false;
    Pose eye[2];
    float eTanX[2] = {1, 1}, eTanY[2] = {1, 1};
    bool eAsym = false;          // eFov valid: exact (asymmetric) per-eye angles left, right, up, down
    float eFov[2][4] = {};
};
void RenderedPublish(const RenderedView& r);
RenderedView RenderedRead();

// quaternion helpers
void QuatMul(const float* a, const float* b, float* out);
void QuatRotate(const float* q, const float* v, float* out);
void QuatFromYaw(float yaw, float* q);
Pose PoseMul(const Pose& a, const Pose& b);   // a * b (b expressed in a's frame)

}
