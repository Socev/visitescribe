#pragma once
#include <cstdint>
typedef void* SemaphoreHandle_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
inline bool xPortInIsrContext() { return false; }
