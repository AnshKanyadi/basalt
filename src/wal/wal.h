// The engine-owned WAL buffer, sync groups, and the two caps. B1-D9.
//
// THE ONE PARAGRAPH THAT DECIDES THE SHAPE (section 2):
//
//   Apply appends a collapsed, fully expanded op list to an ENGINE-OWNED MEMORY
//   BUFFER and makes ZERO Env CALLS. Sync -- called by a different thread --
//   takes the buffer, writes it to the WAL as a SYNC GROUP terminated by a
//   GROUP_END record, fsyncs, and returns the group's high sequence as the new
//   watermark. Recovery replays whole groups and nothing else.
//
// WHY THE BUFFER IS OURS AND NOT WritableFile's. LevelDB's Append flushes to
// the OS when its internal buffer fills, so a write can perform I/O at an
// unpredictable moment -- and "unpredictable moment" is not a way to satisfy
// "never blocks on I/O". Ours is plain memory, so Apply's Env-call count is
// zero by construction and the assertion in env_guard.h can say so.
//
// A GROUP IS THE UNIT OF THREE THINGS AT ONCE, DELIBERATELY:
//   durability -- a Sync covers exactly one group and everything before it;
//   recovery   -- a group is committed whole or not at all;
//   promise    -- DurableSeq advances to a group's high sequence when, and only
//                 when, the Sync covering that group's GROUP_END returns.
// Because all three coincide, THE SET OF REACHABLE RECOVERY POINTS EQUALS THE
// SET OF PROMISED WATERMARKS. That is the answer to ruling 3, and it is
// exactness BY CONSTRUCTION rather than by care.
#ifndef RIFT_WAL_WAL_H_
#define RIFT_WAL_WAL_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "caps.h"
#include "env.h"
#include "env_guard.h"
#include "format.h"
#include "status.h"
#include "writer.h"

namespace rift {
namespace wal {

// Every acquisition of the DB mutex goes through this, so that widening the
// lock's scope widens the guard's scope with it.
//
// THIS IS THE HOUSE MOVE AIMED AT A MUTANT'S STRATEGY RATHER THAN AT A BUG.
// BM16's edit is "widen a lock's scope to simplify a function", and what such an
// edit LEAVES BEHIND is a separate mutex-depth marker still sitting in the old,
// narrow scope -- guarding nothing, reporting nothing, and looking untouched in
// the diff. Binding the marker to the lock means the edit has nothing to leave
// behind: there is no second thing to forget, because there is no second
// thing.
class DbLock {
 public:
  explicit DbLock(std::mutex& m) : lock_(m) {}

 private:
  std::lock_guard<std::mutex> lock_;
  MutexHeldMarker marker_;
};

class Wal {
 public:
  // Creates NNNNNN.log, writes its FILE_HEADER, and makes the directory entry
  // DURABLE before returning (section 7.2 step 6). The last part is not
  // bookkeeping: a WAL created, written and fsynced is still losable if the
  // entry naming it was never made durable -- the bytes survive and the name
  // does not.
  static Status Open(Env* env, const std::string& dir, uint64_t file_number,
                     const Caps& caps, std::unique_ptr<Wal>* out);

  ~Wal();

  // Buffers one batch. MAKES ZERO Env CALLS -- asserted, not asserted-in-prose.
  Status Apply(SeqNum seq, const std::vector<Op>& ops);

  // Writes every buffered batch, terminates the group with a GROUP_END, and
  // fsyncs. The mutex is held ONLY to swap the buffer out; the Append and Sync
  // happen with it released, which is what stops an fsync blocking every
  // reader (section 8.3, and the failure the lock ruling opened).
  Status Sync(SeqNum* watermark);

  SeqNum DurableSeq() const;
  Status Close();

  const Caps& caps() const { return caps_; }
  uint64_t file_number() const { return file_number_; }

 private:
  Wal(const Caps& caps, WritableFilePtr file, uint64_t file_number);

  Caps caps_;
  WritableFilePtr file_;
  std::unique_ptr<LogWriter> writer_;
  uint64_t file_number_;

  mutable std::mutex mu_;
  std::vector<std::string> buffered_;  // encoded BATCH records
  uint64_t buffered_bytes_ = 0;        // by the FROZEN formula, not by encoding
  SeqNum high_seq_ = 0;
  SeqNum durable_ = 0;
  bool closed_ = false;
};

}  // namespace wal
}  // namespace rift

#endif  // RIFT_WAL_WAL_H_
