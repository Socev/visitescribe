#pragma once
#include <cstdarg>
typedef int (*vprintf_like_t)(const char*, va_list);
inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t) { return nullptr; }
