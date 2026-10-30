#pragma once
#include <windows.h>
namespace mea {
void CamScanRequest(UINT renderW, UINT renderH);   // arm: catch the next per-view constant block, then scan memory
bool CamScanArmed();
void CamScanInspect(const void* data, size_t bytes);   // from the Unmap hook, before the real Unmap
void CamScanOnPresent();
void CamScanSetAutoWatch(int seconds);                 // after the next scan, watch the writers of the best hits                               // starts the memory scan thread once a block was caught
}
