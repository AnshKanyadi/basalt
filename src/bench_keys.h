// THE WORKLOAD'S KEY STREAM, SPECIFIED ONCE AND WRITTEN TWICE.
//
// The boundary cost is the difference between two columns of one table, and it
// is a difference only if both columns did the same work. That means the same
// keys in the same order -- not merely the same distribution, which would leave
// any gap attributable to one run having had a luckier access pattern.
//
// So this is splitmix64: eight lines, no state beyond a counter, and
// reimplementable by inspection rather than by trust. The pinned outputs below
// are asserted, so a divergence is a test failure and not a number that is
// quietly wrong.
//
//   PINNED, seed 1: 0x910A2DEC89025CC1 0xBEEB8DA1658EEC67 0xF893A2EEFB32555E
//
// It is NOT rig/rng.h. That is a real PCG64 with its own pinned vectors, and
// using it would mean asserting that two independent implementations agree
// stream-for-stream -- a claim worth making for the corpus and not worth
// making for a benchmark's key order.
#ifndef BASALT_BENCH_KEYS_H_
#define BASALT_BENCH_KEYS_H_

#include <cstdint>
#include <cstdio>
#include <string>

namespace basalt {

inline uint64_t Bench64(uint64_t seed, uint64_t i) {
  uint64_t z = seed + (i + 1) * 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// A fixed-width zero-padded decimal, so keys sort the way a reader expects and
// key length is a constant of the workload rather than a function of the value.
inline std::string BenchKey(uint64_t seed, uint64_t i, int width) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%0*llu", width,
                (unsigned long long)(Bench64(seed, i) % 100000000000000ull));
  return std::string(buf, width);
}

}  // namespace basalt

#endif  // BASALT_BENCH_KEYS_H_
