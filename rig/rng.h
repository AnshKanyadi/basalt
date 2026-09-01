// PCG64, PROJECT-OWNED, WITH PINNED VECTORS -- the C++ half of `[A1]`.
//
// The amendment's reason: a standard library's compatibility promise for its
// convenience mappings is too weak to hang a permanent seed corpus on, and A
// SILENT CHANGE WOULD LEAVE EVERY CORPUS ENTRY SELF-CONSISTENT BUT DIFFERENT.
//
// C++ has that problem with a different name. `std::mt19937` is
// standardised, but the DISTRIBUTIONS are not -- `std::uniform_int_distribution`
// may produce different values on two conforming libraries from one engine and
// one seed. A corpus keyed to a seed cannot rest on that.
//
// So: the generator is here, the mapping to a range is here, and both are
// pinned by known-answer tests. THE SEED CORPUS RESTS ON THIS FILE AND ON
// NOTHING THE TOOLCHAIN SUPPLIES.
#ifndef RIFT_RIG_RNG_H_
#define RIFT_RIG_RNG_H_

#include <cstdint>

namespace rift {
namespace rig {

class Pcg64 {
 public:
  explicit Pcg64(uint64_t seed) { Seed(seed); }

  void Seed(uint64_t seed) {
    state_ = 0;
    inc_ = (kDefaultStream << 1u) | 1u;
    Next();
    state_ += seed;
    Next();
  }

  uint64_t Next() {
    const __uint128_t old = state_;
    state_ = old * kMultiplier + inc_;
    // XSL-RR: xor the halves, rotate by the top bits.
    const uint64_t xorshifted =
        static_cast<uint64_t>((old >> 64) ^ old);
    const unsigned rot = static_cast<unsigned>(old >> 122);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 63));
  }

  // UNIFORM ON [0, n), BY REJECTION, and the rejection is the point: a modulo
  // would be biased and the bias would be a silent property of the corpus.
  uint64_t Below(uint64_t n) {
    if (n <= 1) return 0;
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % n) - 1;
    uint64_t v = Next();
    while (v > limit) v = Next();
    return v % n;
  }

  // A DERIVED SUB-STREAM, named like `internal/rng`'s. Two consumers that share
  // a generator interleave, so a change to one changes the other's values --
  // which is how a corpus entry stops reproducing for a reason nobody can see.
  Pcg64 Derive(uint64_t label) const {
    Pcg64 d(0);
    d.state_ = state_ ^ (label * 0x9E3779B97F4A7C15ull);
    d.inc_ = (((label << 1u) | 1u) ^ inc_) | 1u;
    d.Next();
    return d;
  }

 private:
  static constexpr __uint128_t kMultiplier =
      (static_cast<__uint128_t>(2549297995355413924ull) << 64) |
      4865540595714422341ull;
  static constexpr uint64_t kDefaultStream = 1442695040888963407ull;

  __uint128_t state_ = 0;
  __uint128_t inc_ = 0;
};

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_RNG_H_
