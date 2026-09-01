#include "tower.h"

namespace basalt {

uint64_t Fnv1a64(Slice key) {
  uint64_t h = 0xcbf29ce484222325ULL;
  const char* p = key.data();
  for (std::size_t i = 0; i < key.size(); ++i) {
    h ^= static_cast<uint64_t>(static_cast<unsigned char>(p[i]));
    h *= 0x100000001b3ULL;
  }
  return h;
}

namespace {
int CountTrailingZeros(uint64_t x) {
  if (x == 0) return 64;
  int n = 0;
  while (((x >> n) & 1ULL) == 0ULL) ++n;
  return n;
}
}  // namespace

int TowerHeight(Slice key) {
  const int ntz = CountTrailingZeros(Fnv1a64(key));
  const int h = 1 + (ntz / 2);
  return h < kMaxHeight ? h : kMaxHeight;
}

}  // namespace basalt
