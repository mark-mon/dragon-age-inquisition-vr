#pragma once
#include <cstdint>
#include <string>
namespace mea {
uint8_t* FbInstance(const char* cls);                  // live settings object, or nullptr
bool FbSet(const char* line);                          // "Class.Field value"
std::string FbGet(const char* cls, const char* field); // current value as text ("" if unavailable)
void FbOnPresent(uint64_t presentIndex);               // startup cfg + trigger files
}
