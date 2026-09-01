// The WAL writer: logical records in, fragments out.
//
// B1-D3(c), RULED: LevelDB-shaped 32 KiB blocks with fragmentation, PLUS an
// explicit sync-group terminator in the record stream.
//
// The axis that decided it was not space, it was RESYNCHRONIZATION. Under flat
// length-prefixed records, a corrupt LENGTH field makes every later byte
// unparseable, so recovery cannot tell "the log ends here" from "twenty valid
// records follow and I can no longer find them". Section 5.4's rule depends
// entirely on telling those apart, so flat framing does not lose a nicety -- it
// makes the torn-tail rule UNSAFE, because the safe-looking behaviour (stop at
// the first bad record) silently discards promised data.
//
// Blocks buy the discrimination: damage is bounded to one block and recovery
// can always advance to the next block boundary and ask whether anything valid
// lives there. Cost: 7 bytes per fragment plus up to 7 bytes of block padding.
//
// The GROUP_END terminator is what blocks alone do not provide: ATOMICITY OF A
// SYNC GROUP AT RECOVERY. Without it a torn Sync leaves recovery landing on
// whichever BATCH boundary happened to survive, so the oracle's expected
// recovery point is "any of the k batch boundaries inside the in-flight group"
// and ruling 3's comparison stops being exact. With it the expected set
// collapses to two known values. THE GROUP MARKER IS WHAT TURNS A RANGE CHECK
// INTO AN EQUALITY CHECK -- that is its whole justification.
//
// Also rejected, and recorded because it is the obvious alternative: a small
// "durable extent" file fsynced after each group. It is correct and encodes the
// tail/interior boundary directly, and it DOUBLES THE FSYNCS ON THE COMMIT
// PATH. A 2x write-latency tax to simplify an oracle's arithmetic is the wrong
// trade in a database.
#ifndef BASALT_WAL_WRITER_H_
#define BASALT_WAL_WRITER_H_

#include <cstdint>

#include "basalt/env.h"
#include "basalt/format.h"
#include "basalt/slice.h"
#include "basalt/status.h"

namespace basalt {
namespace wal {

class LogWriter {
 public:
  explicit LogWriter(WritableFile* file) : file_(file) {}

  // Appends one logical record, fragmenting it across block boundaries.
  //
  // Makes Env calls, so it is NEVER called from Apply. The syncer owns it.
  Status AddRecord(Slice payload);

  uint64_t offset() const { return offset_; }

 private:
  Status EmitFragment(FragmentType type, Slice payload);

  WritableFile* file_;
  uint64_t offset_ = 0;
};

}  // namespace wal
}  // namespace basalt

#endif  // BASALT_WAL_WRITER_H_
