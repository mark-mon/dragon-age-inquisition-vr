#pragma once
#include <cstdint>
namespace mea {
void WatchOnPresent(uint64_t presentIndex);   // polls MEAVR\watch.trigger, starts / stops hardware write watchpoints
}
