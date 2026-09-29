#pragma once
#include <cstdint>
inline void esp_efuse_mac_get_default(uint8_t* m) {
  const uint8_t mac[6] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};
  for (int i = 0; i < 6; ++i) m[i] = mac[i];
}
