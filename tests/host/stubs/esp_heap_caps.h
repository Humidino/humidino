#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_INTERNAL = 0;
inline size_t heap_caps_get_free_size(int) { return 100000; }
