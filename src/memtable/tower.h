// Tower heights: a pure function of the key, and nothing else.
//
// B1-D6b, RULED. LevelDB draws skiplist heights from a PRNG. We do not, and the
// reason is Amendment A5 plus DR-12's argument transferring: engine/model's
// treap priorities come from fnv1a64(key) rather than from an RNG so that
// engine internals stay decoupled from any random stream.
//
// It buys more than determinism. The same key always gets the same height, so
// THE STRUCTURE IS A PURE FUNCTION OF THE KEY SET -- and a shape-dependent bug
// reproduces from the workload alone, with no seed to carry and no reseed to
// get wrong.
//
// THE COST, ACCEPTED AND WRITTEN DOWN RATHER THAN DISCOVERED AT B5:
//
//   A degenerate key set is degenerate PERMANENTLY. If a key set maps to a
//   pathological tower distribution it does so on every machine, in every run,
//   forever. There is no reseed, because reproducibility from the key set alone
//   is the property we chose and a reseed is exactly what would destroy it.
//
//   The function is public knowledge, so the degenerate set can be CONSTRUCTED.
//   fnv1a64 and the mapping below are in the design document. An adversary who
//   chooses keys -- and in a KV database, clients choose keys -- can force
//   towers of height 1 and turn the expected O(log n) into O(n).
//
//   This is a PERFORMANCE property, not a safety one. No invariant depends on
//   tower height: ordering, visibility, snapshots and recovery are all
//   height-independent. The consequence of the attack is a slow memtable, not a
//   wrong one.
//
//   The fix, named and declined: a per-DB salt mixed into the hash. It defeats
//   the constructed key set and costs exactly the property we bought -- the
//   shape would become a function of (key set, salt), so the same keys in a
//   different DB build a different structure and a shape-dependent bug stops
//   reproducing from the workload alone. Declined for v1. The upgrade path, if
//   a fuzzer or a real workload ever makes it matter: derive the salt from the
//   DB's creation file number and record it in B2's manifest.
//
// THE MAPPING IS PINNED BY GOLDEN VECTORS (TestHeightVectors). The memtable's
// shape is now on-disk-adjacent behaviour, so any change to it must FAIL A
// VECTOR to happen. Per A0's rule about signed packages, the vectors never
// change in the same commit as the code they pin.
#ifndef BASALT_MEMTABLE_TOWER_H_
#define BASALT_MEMTABLE_TOWER_H_

#include <cstddef>
#include <cstdint>

#include "basalt/slice.h"

namespace basalt {

inline constexpr int kMaxHeight = 12;

// FNV-1a, 64-bit. The same function engine/model uses for treap priorities.
uint64_t Fnv1a64(Slice key);

// height = 1 + min(ntz(fnv1a64(key)) / 2, kMaxHeight - 1)
//
// Dividing the trailing-zero count by two gives a branching factor of 4, which
// is LevelDB's kBranching; taking it from the hash rather than from a coin
// makes it reproducible.
int TowerHeight(Slice key);

}  // namespace basalt

#endif  // BASALT_MEMTABLE_TOWER_H_
