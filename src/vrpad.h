#pragma once
#include <openxr/openxr.h>
#include <vector>
namespace mea {
// VR controllers -> XInput gamepad 1 (see vrpad.cpp for the mapping).
XrActionSet VrPadCreate(XrInstance inst);          // before xrAttachSessionActionSets
void VrPadSessionReady(XrSession s);                 // after the action sets are attached
XrActionSet VrPadSet();
void VrPadUpdate(XrSession s, XrSpace ref, XrTime t, bool focused);   // per frame, after xrSyncActions
bool VrPadActive();
bool VrPadAimPose(XrSpace ref, XrTime t, int hand, XrPosef& pose);    // controller aim pose in ref space
}
