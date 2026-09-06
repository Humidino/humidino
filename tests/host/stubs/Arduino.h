#pragma once
#include <cstdint>
#include <cstdio>
inline uint32_t testNow = 0;
inline int testRelayLevel = 0;
constexpr int HIGH = 1, LOW = 0, OUTPUT = 1;
inline uint32_t millis() { return testNow; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int level) { testRelayLevel = level; }
