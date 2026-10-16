#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
inline unsigned long millis() { return 1000; }
inline long random(long low, long high) { return high > low ? low : 0; }
