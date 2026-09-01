// THE TORN-TAIL RULE. B1-D4, RULED: (d), RESYNC-VERIFIED.
//
//   A recovery read that fails -- bad CRC, truncated header, truncated payload,
//   a length running past its block, or an illegal fragment transition --
//   TERMINATES THE LOG AT THAT POINT. Groups already closed by a GROUP_END
//   stand; any BATCH records after the last GROUP_END, and any incomplete
//   logical record, are DISCARDED. This is not an error and is not reported as
//   one.
//
//   Recovery then RESYNCHRONIZES: it advances to the next block boundary and
//   scans forward for a STRUCTURALLY VALID record -- CRC-valid, type in {FULL,
//   FIRST}, kind in {BATCH, GROUP_END}, and carrying a sequence GREATER than
//   the last committed group's. If one is found, the log is CORRUPT IN THE
//   INTERIOR: the open FAILS, reporting file, block, byte offset, and the
//   sequence of the last committed group. NO SILENT TRUNCATION, EVER.
//
// WHY THE DISTINCTION IS SAFE, in four steps, because one of them is where the
// argument ends:
//
//   1. A torn record lies strictly after the last durable GROUP_END. Under
//      B1-D2 a file's durable image advances only when a Sync returns, and a
//      Sync covers a whole group ending in its GROUP_END. A torn record is by
//      definition partially written, so it was in no returned Sync's extent.
//   2. Therefore discarding the tail never discards a promised byte. R >= W,
//      the safety-critical direction: committed is forever.
//   3. Recovery commits only complete groups, so R is a group boundary, and the
//      highest one that can exist on disk is the group whose Sync was in flight
//      at the kill. So R is in {W, G_inflight} -- section 7.4's two-element set.
//   4. So a valid record can follow an invalid one ONLY IF A PREMISE FAILED. A
//      single append-only file is written in offset order and durability is
//      prefix-closed, so a crash cannot produce a valid record after a torn
//      one. Media corruption can, and a device that reordered across an fsync
//      can. Both falsify step 1 -- and step 1 is what makes truncation safe.
//      WHEN THE PREMISE FAILS, TRUNCATION IS NO LONGER SAFE, SO RECOVERY MUST
//      NOT TRUNCATE. That is the whole argument for (d) over (b), and why the
//      response is a hard error rather than a best effort.
//
// WHAT WAS REJECTED, and (c) is the one worth remembering:
//   (a) every checksum failure is fatal -- turns the most common real-world
//       event, a crash during a write, into an outage while buying nothing.
//   (b) every checksum failure is end-of-log -- correct whenever the failure
//       really is the tail, and SILENTLY discarding promised data whenever it
//       is not. Silently is the operative word: no log line, no error, no
//       metric; the database opens, is short some committed writes, and nobody
//       learns for weeks.
//   (c) position-based without resynchronization -- sounds like (d) and is not.
//       "Appears to be the last record" is undecidable without resync: a
//       corrupt length leaves recovery unable to locate the next record, so
//       under (c) every corrupt length is classified as a tail. (C) IS (B)
//       WEARING A BETTER NAME.
//
// THE CHAIN IS A TWO-STATE MACHINE AND ITS TRANSITIONS ARE PART OF THE FROZEN
// FORMAT (section 5.4.2). An illegal transition is a read failure of the same
// kind as a bad CRC and feeds the same rule -- one rule extended, not two rules
// coexisting.
//
//   OUTSIDE --FULL-->   OUTSIDE      (a complete single-fragment record)
//   OUTSIDE --FIRST-->  INSIDE
//   INSIDE  --MIDDLE--> INSIDE
//   INSIDE  --LAST-->   OUTSIDE      (a complete multi-fragment record)
//
//   every other transition is ILLEGAL:
//     OUTSIDE --MIDDLE-->  |  OUTSIDE --LAST-->
//     INSIDE  --FULL-->    |  INSIDE  --FIRST-->
#ifndef BASALT_WAL_READER_H_
#define BASALT_WAL_READER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "basalt/format.h"
#include "basalt/slice.h"

namespace basalt {
namespace wal {

enum class ScanOutcome : uint8_t {
  // The log ended on a record boundary. Nothing was discarded.
  kCleanEnd,
  // A read failed and nothing structurally valid follows. Discard and open.
  kTornTail,
  // A read failed and something structurally valid DOES follow, or two
  // structurally valid fragments formed an illegal transition. Open FAILS.
  kInteriorCorruption,
};
const char* ScanOutcomeName(ScanOutcome outcome);

struct LogicalRecord {
  RecordKind kind = RecordKind::kInvalid;
  std::string payload;
  uint64_t offset = 0;  // byte offset of the fragment that started it
};

struct ScanResult {
  ScanOutcome outcome = ScanOutcome::kCleanEnd;

  // Every complete logical record read before the failure or EOF.
  std::vector<LogicalRecord> records;

  // How many of `records` are COMMITTED: everything up to and including the
  // last GROUP_END. Records after it are the discarded tail, and section
  // 5.4.1 says discarding them is not an error and is not reported as one.
  std::size_t committed_count = 0;
  SeqNum last_committed_seq = 0;

  // Meaningful unless kCleanEnd. Reported at full precision because a refused
  // open that cannot say WHERE is a refused open nobody can act on.
  uint64_t failure_offset = 0;
  uint64_t failure_block = 0;
  std::string failure_reason;

  // Meaningful only for kInteriorCorruption: where the structurally valid
  // record was found, or the offset of the illegal transition.
  uint64_t resync_offset = 0;
};

// Scans a whole WAL image. Pure: bytes in, verdict out, no Env, no engine.
//
// That purity is why section 14.4 splits the reader from recovery: every gate
// here is drivable from hand-built byte images, which makes this the cheapest
// place in the sequence to induce failures exhaustively -- and it is why these
// gates land BEFORE the writer is trusted rather than after.
ScanResult ScanLog(Slice image);

}  // namespace wal
}  // namespace basalt

#endif  // BASALT_WAL_READER_H_
