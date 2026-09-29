#pragma once
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { static int m; return &m; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, int) { return pdTRUE; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { return pdTRUE; }
