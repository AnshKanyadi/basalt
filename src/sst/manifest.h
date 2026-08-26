// THE MANIFEST. B2-D4(c), approved: a WAL-FRAMED LOG, plus CURRENT.
//
// Same 32 KiB blocks, same fragment chain, same CRC-covering-the-length, same
// section 5.4 torn-tail rule, read by the same ScanLog that B1.7a induced
// against hand-built bytes before any writer existed. B2 already freezes one
// new format -- the SSTable -- and freezing a SECOND one here would mean a
// third encoding, a third torn-tail rule and a third classifier to keep induced.
// Section 7.5's one mechanism, two users, applied to a format.
//
// ---------------------------------------------------------------------------
// D7'S FORWARD BINDING, AND THE ONE PLACE THIS FILE DIVERGES FROM ITS LETTER.
//
//   The manifest may record which files exist; it may NEVER record a durable
//   sequence the WAL cannot independently justify.
//
// Mechanism 1 as written: "no manifest record has a watermark field ... enforced
// by there being nothing to write it into."
//
// REPORTED, NOT ADAPTED: reusing the WAL's framing means reusing its GROUP_END
// terminator, and GROUP_END carries a `high_seq`. So it is NOT true that no
// record in this file has a sequence-shaped field. What is true, and is what
// the mechanism was for:
//
//   * NO MANIFEST EDIT HAS ONE. EditKind's payloads carry file numbers, sizes,
//     key bounds and a per-table largest sequence -- and nothing else.
//   * ManifestState HAS NOWHERE TO PUT ONE. There is no watermark field in the
//     replayed state, so a number arriving in a GROUP_END has no destination.
//   * A MANIFEST GROUP_END WITH A NON-ZERO high_seq FAILS THE OPEN. The writer
//     writes zero; the reader refuses anything else. Both directions asserted.
//
// The per-table `largest_seq` is the deliberate exception D4 section 5.1 point 2
// provides for, and it is not taken on trust: OPEN RE-DERIVES IT from the
// table's own largest internal key and refuses the open on disagreement. The
// manifest is never the sole authority for any number in it.
//
// ---------------------------------------------------------------------------
// CURRENT names the live manifest and is replaced by write-temp, sync, rename,
// directory sync. That is where Env::RenameFile stops being declared-and-unused
// and where section 3.3's "missing directory sync around an atomic rename"
// injector finally has something to find.
//
// ---------------------------------------------------------------------------
// WHAT B2 ADDS TO B1'S FROZEN WAL FORMAT, AND WHAT IT DOES NOT.
//
// ADDED: one enumerator, `RecordKind::kManifestEdit`. No WAL has ever contained
// it and no WAL ever writes it; WAL recovery REFUSES it, and manifest replay
// refuses `kBatch` in the same breath. -Werror=switch is what makes that a
// build failure at every site that decides rather than a rule to remember.
//
// NOT CHANGED: the magic, the version, the fragment header, the CRC's coverage,
// the block size, the torn-tail rule. EVERY BYTE OF EVERY EXISTING WAL DECODES
// IDENTICALLY. A manifest is distinguished from a WAL by the kinds it holds,
// which needs no second magic and therefore no parameterised file header.
//
// ---------------------------------------------------------------------------
// EVERY Open WRITES A NEW MANIFEST AND SWAPS CURRENT.
//
// A WritableFile truncates, so appending to the manifest that was just replayed
// is not available; and a rotation on every open is what makes the rename and
// its directory sync a path the crash rig visits on EVERY schedule rather than
// on the rare one that happened to flush.
//
// Cost, stated: the manifest is rewritten whole at each open, O(live tables).
// At B2 that is bounded by the flush threshold and the number of files; the
// measurement that would reopen it is B5's open latency against table count.
#ifndef RIFT_SST_MANIFEST_H_
#define RIFT_SST_MANIFEST_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "env.h"
#include "internal_key.h"
#include "manifest_format.h"
#include "slice.h"
#include "status.h"
#include "table.h"
#include "writer.h"

namespace rift {
namespace sst {

class Manifest {
 public:
  // Replays the manifest named by CURRENT, or creates a fresh one if CURRENT is
  // absent. Re-derives every named table's largest sequence, size and key
  // bounds from the table ITSELF and REFUSES THE OPEN on disagreement.
  //
  // `tables`, if not null, receives the tables it opened in order to do that,
  // NEWEST FIRST. They are handed out rather than reopened because verification
  // and use would otherwise read and validate every file twice -- and because
  // the copy a caller reads from should be the copy that was checked.
  static Status Open(Env* env, const std::string& dir, ManifestState* state,
                     std::vector<std::shared_ptr<Table>>* tables,
                     std::unique_ptr<Manifest>* out);

  ~Manifest();
  Manifest(const Manifest&) = delete;
  Manifest& operator=(const Manifest&) = delete;

  // Appends the edits as ONE sync group, terminated by a GROUP_END with a zero
  // sequence, and Syncs. THE CALLER PERFORMS THE DIRECTORY SYNC: B2-D5's
  // ordering is the crash-consistency claim and it does not belong inside a
  // writer that can only see its own file.
  Status AppendGroup(const std::vector<ManifestEdit>& edits);

  uint64_t number() const { return number_; }
  Status Close();

 private:
  Manifest(uint64_t number, WritableFilePtr file);

  uint64_t number_;
  WritableFilePtr file_;
  std::unique_ptr<wal::LogWriter> writer_;
};

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_MANIFEST_H_
