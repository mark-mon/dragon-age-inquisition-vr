#pragma once
#include <cstdint>
namespace mea {
struct Pose;
// Head aim: the game's camera yaw / pitch control values follow the head while active, so the game aims
// (and draws its reticle) where you look. The render then applies only the rest of the head rotation.
void HeadAimSetTarget(int kind, uintptr_t addr, int sign, bool deg);   // kind 0 yaw, 1 pitch (found by aimtest)
bool HeadAimHasTarget();
void HeadAimUpdate(const float rayOrigin[3], const float rayDir[3], bool aimHeld);   // per XR frame: aim ray (head or controller) in the recentre reference frame, real m
float HeadAimHudDistance();   // where the reticle goes while engaged (the aim point's distance, or hud_aim_distance)
// Engine, once per rendered frame with the game's own camera rows (right, up, back, pos): measures how far the game
// camera turned because of head aim and turns the head pose used for the eye cameras into the part the game camera
// does not contain. Returns false (pose untouched) when head aim is not engaged.
bool HeadAimCompensate(const float* gameCam, Pose& head, float dPos[3], bool measure);   // dPos: world move of the camera position
bool HeadAimActive();
bool HeadAimFirstPerson();   // first person currently active (gameplay)
void HeadAimCullPose(Pose& head);   // the head pose relative to the game camera as head aim has turned it (last frame)
// While head aim is engaged: the game camera's rotation relative to the body view (reference frame, OpenXR axes).
// The HUD panel is placed along it so the game's reticle and world markers line up with the world.
// origin: the game camera's position (where its aim ray starts) relative to the rendered body camera, in the
// reference frame (real metres) - the reticle is placed on that ray.
bool HeadAimHudRotation(float q[4], float origin[3]);
}
