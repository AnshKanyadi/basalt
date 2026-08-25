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

// WHAT RECOVERY MUST BE TOLD, once the WAL is no longer the only durable store.
//
// B1 derived both of these and B2 cannot. The fresh WAL was `max+1` over the
// directory, and nothing was ever already covered because nothing was ever
// flushed. B2-D5: "max+1 numbering expires here, exactly as B1-D7 said it
// would" -- once a flushed WAL is deleted the highest surviving number is no
// longer the highest ever issued, so the number comes from the manifest.
struct RecoverOptions {
  // The number for the fresh WAL this call creates. From the manifest.
  uint64_t next_file_number = 1;
  // S: the highest sequence the SSTables already hold. A replayed batch at or
  // below it is a CORRUPTION and not a skip -- see the partition invariant.
  SeqNum covered_through = 0;
  // THE LIVE WALs, BY NUMBER, ASCENDING, from the manifest. Exactly these are
  // read. A named WAL that is absent is a refused open, with NO EXCEPTION; a
  // present WAL that is not named must hold no committed batches, and is
  // ignored. See manifest.h for why creating before naming is what makes both
  // halves true without one.
  //
  // Empty means "read every WAL present", which is what a caller with no
  // manifest gets. Only the WAL's own tests do that.
  std::vector<uint64_t> named_wals;
};

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

  // WALs below `log_number`: covered by a table, obsolete, and NOT read. The
  // count is reported so that "nothing is covered twice" is an asserted number
  // rather than an intention -- it is the one direction of the partition that
  // holds by FILE SELECTION rather than by a sequence comparison.
  std::size_t obsolete_wals = 0;

  // The lowest and highest batch sequences found across the replayed WALs, or
  // zero if none. Reported for the same reason: a checker that reports nothing
  // is a checker nobody can test.
  SeqNum first_wal_seq = 0;
  SeqNum last_wal_seq = 0;
};

// Section 7.2, in order:
//   1. acquire LOCK
//   2. GetChildren, parse NNNNNN.log, SORT BY PARSED NUMBER -- never directory
//      order, never mtime
//   3. B2-Q1: assert the RECOVERY INTERVALS PARTITION [1, W] EXACTLY.
//
//      B1 asserted that WAL file numbers were gapless, and that was a property
//      of the WAL being the only durable record: no file was ever deleted, so a
//      gap meant a LOST DIRECTORY ENTRY. B2 deletes flushed WALs, so
//      gaplessness is false by design and the check is REPLACED rather than
//      retired. A retired check is a check nobody is watching; a replaced one
//      has to be shown to cover what the old one covered.
//
//      Let W be the recovered watermark and S = options.covered_through, the
//      highest sequence the SSTables hold. Recovery contributes [1, S] from the
//      tables and (S, W] from the surviving WALs. THOSE INTERVALS MUST PARTITION
//      [1, W] EXACTLY: nothing covered twice, nothing missing.
//
//      NOTHING COVERED TWICE is a question about SEQUENCES and is checked as
//      one: the manifest names only the WALs no table covers, so a batch
//      arriving at or below S means file selection failed, and it is a refused
//      open rather than a skipped record. Repairing it by skipping would hide
//      exactly the thing worth knowing.
//
//      NOTHING MISSING IS A QUESTION ABOUT FILES AND CANNOT BE ANSWERED WITH
//      SEQUENCES. A Write whose Apply is refused by a cap CONSUMES ITS SEQUENCE
//      AND WRITES NO BATCH -- deliberately; the contract requires monotonicity,
//      not density -- so a legitimate hole in the sequence space is
//      indistinguishable from a lost file, and a density check would refuse the
//      normal case in the name of the abnormal one. The first draft of this
//      invariant made exactly that mistake. It is answered instead with file
//      identities, against the set of WALs the manifest names: see
//      manifest.h's `wals`. What sequences still buy is ORDER -- each file's
//      first batch must exceed the previous file's last -- and the join with
//      the tables, S < first.
//
//   4. replay each file in order, committing group by group
//   5. recovered_seq = the highest committed GROUP_END.high_seq; monotone
//      across files
//   6. create WAL max+1 and Directory::Sync BEFORE returning
//   7. DurableSeq = VisibleSeq = recovered_seq
// DOES NOT ACQUIRE THE DIRECTORY LOCK. B1's Recover took it, recovered, and
// released it. B2 must read the MANIFEST under that same lock -- the manifest
// supplies both fields of RecoverOptions -- and a function that both locks and
// recovers cannot be composed with one that has to run inside the lock. So the
// caller locks; DB::Open is the caller.
Status Recover(Env* env, const std::string& dir, const Caps& caps,
               const RecoverOptions& options, RecoveryResult* out);

// The path of WAL `number` under `dir`. Exported because the flush deletes a
// retired WAL by name and must not build that name a second way.
std::string LogPath(const std::string& dir, uint64_t number);

}  // namespace wal
}  // namespace rift

#endif  // RIFT_WAL_RECOVERY_H_
