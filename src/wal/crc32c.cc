#include "crc32c.h"

#include <array>

namespace basalt {
namespace wal {
namespace {

constexpr uint32_t kPoly = 0x82F63B78u;

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (kPoly ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Crc32c(const char* data, std::size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    const uint8_t b = static_cast<uint8_t>(data[i]);
    crc = kTable[(crc ^ b) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace wal
}  // namespace basalt
