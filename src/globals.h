#pragma once
#include <windows.h>
#include <string>
#include "config.h"

namespace mea {
struct Globals {
    std::wstring gameDir, outDir;
    Config cfg;
    HANDLE initEvent = nullptr;
};
Globals& G();
}
