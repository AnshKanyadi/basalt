// Recovery: the log is the single authority, and the watermark is READ, never
// inferred.
//
// B1-D7, RULED: NO MANIFEST IN B1. There are no SSTables, so no version state
// to be inconsistent with, and a manifest recording a durable sequence would be
// A SECOND AUTHORITY ON THE WATERMARK THAT COULD DISAGREE WITH THE LOG -- the
// exact shape of the A0.5 bug, rebuilt in C++. `recovered_seq` is a fact about
// bytes: derived, never stored.
//
// FORWARD BINDING TO B2: the manifest may record which files exist; it may
// NEVER record a durable sequence the WAL cannot independently justify. And
// max+1 numbering stops being safe the moment B2 deletes a flushed WAL, which
// is where the file-number counter moves into the manifest.
//
// ---------------------------------------------------------------------------
// THE WATERMARK IS RECORDED, NEVER INFERRED FROM THE SHAPE OF ANYTHING.
//
// It is GROUP_END.high_seq, read out of the record. It is NOT the highest
// sequence seen among BATCH records, not the last batch's sequence, not a
// count, and not anything reconstructed from what recovery happened to find.
// Track A's BUG-005 was a watermark inferred from the shape of a structure
// rather than read from where it was written, and it cost three cycles.
//
// The two are almost always equal, which is exactly why the distinction has to
// be enforced by a test rather than by intent: a probe image in which they
// DIFFER is the only thing that can tell which one an implementation is using.
//
// ---------------------------------------------------------------------------
// THE EXACTLY-AT-WATERMARK CONTRACT IS ASSERTED HERE DIRECTLY.
//
// Ruling 3: crash recovery yields exactly the state at the durable watermark,
// for any watermark the sync-completion schedule can produce. The tests assert
// that against THE HARNESS'S OWN RECORD of what it submitted and which Sync
// returned -- never by checking that recovery agrees with a second
// implementation. Agreement between two paths is not the same as either path
// being right (section 13.4b), and two paths that share an assumption agree
// most confidently exactly where they are both wrong.
#ifndef RIFT_WAL_RECOVERY_H_
#define RIFT_WAL_RECOVERY_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "caps.h"
#include "env.h"
#include "memtable.h"
#include "status.h"
#include "wal.h"

namespace rift {
namespace wal {

struct RecoveryResult {
  std::unique_ptr<MemTable> table;

  // GROUP_END.high_seq of the last committed group, read from the record.
  SeqNum recovered_seq = 0;

  // The freshly created WAL, at max+1, with its directory entry already durable.
  std::unique_ptr<Wal> wal;

  // What recovery actually saw, for the report a refused open cannot give.
  std::vector<uint64_t> file_numbers;
  std::size_t committed_batches = 0;
  std::size_t discarded_batches = 0;  // after the last GROUP_END; not an error
};

// Section 7.2, in order:
//   1. acquire LOCK
//   2. GetChildren, parse NNNNNN.log, SORT BY PARSED NUMBER -- never directory
//      order, never mtime
//   3. assert numbering is GAPLESS
//   4. replay each file in order, committing group by group
//   5. recovered_seq = the highest committed GROUP_END.high_seq; monotone
//      across files
//   6. create WAL max+1 and Directory::Sync BEFORE returning
//   7. DurableSeq = VisibleSeq = recovered_seq
Status Recover(Env* env, const std::string& dir, const Caps& caps,
               RecoveryResult* out);

}  // namespace wal
}  // namespace rift

#endif  // RIFT_WAL_RECOVERY_H_
