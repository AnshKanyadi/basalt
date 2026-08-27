// AN SSTABLE, OPEN FOR READING.
//
// THE WHOLE FILE IS RESIDENT. IT IS NO LONGER A REQUIREMENT, AND THAT IS THE
// CHANGE B3.5 MADE -- stated here rather than left as a stale justification.
//
// B2-D7 §8 point 1 made it a requirement: `DeleteRange` expanded at Apply, Apply
// makes NO Env call, and the expansion had to read the merged view -- so every
// live table had to be readable without touching Env. A reader that pulled
// blocks on demand would have put a syscall inside Apply.
//
// B3.5 RETIRED THE EXPANSION. A range deletion is now one entry whose meaning
// does not depend on the state around it, so `Apply` reads nothing and nothing
// on the Apply path needs a resident table.
//
// WHAT STILL DEPENDS ON RESIDENCY, STATED HONESTLY, because "no longer required
// by X" is not "no longer required":
//
//   * A SNAPSHOT OR ITERATOR OUTLIVING A COMPACTION -- RETIRED AT B3.6, AND
//     THIS IS WHAT THE ENTRY IS FOR. It used to read through tables whose FILES
//     had been deleted, which worked only because the bytes were in memory:
//     correctness by an argument whose premise moves. `db.cc` now keeps the
//     FILE alive until the last reader drops its `shared_ptr`, so the read is
//     correct because the file is there, not because the bytes happen to be.
//   * `Table::NewestCovering` and the point path read blocks without an Env
//     call, which every caller currently assumes. STILL TRUE, and it is what a
//     block cache would have to preserve.
//
// So residency is now a PERFORMANCE property and nothing else: no correctness
// claim in this engine rests on it. The cost is unchanged -- memory grows with
// the live data set -- and the measurement that would move it is B5's, against
// a workload whose working set exceeds it.
#ifndef RIFT_SST_TABLE_H_
#define RIFT_SST_TABLE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bloom.h"
#include "env.h"
#include "internal_key.h"
#include "range_tombstone.h"
#include "internal_iter.h"
#include "slice.h"
#include "status.h"
#include "table_check.h"
#include "table_format.h"

namespace rift {
namespace sst {

class Table {
 public:
  // Reads the file and VALIDATES IT with the classifier before it is usable.
  // Every table this process reads has been through the same rules that were
  // induced against hand-built bytes, so a damaged table is a refused open and
  // not a wrong answer.
  static Status Open(Env* env, const std::string& path, uint64_t number,
                     std::shared_ptr<Table>* out);

  uint64_t number() const { return number_; }

  // The sequence of the newest range tombstone at or below `snapshot` covering
  // `user_key`, or 0 if none does. Parsed once at Open, from the same block the
  // classifier already validated.
  SeqNum NewestCovering(Slice user_key, SeqNum snapshot) const;

  const std::vector<RangeTombstone>& tombstones() const { return tombstones_; }
  uint64_t file_bytes() const { return image_.size(); }
  const TableCheck& check() const { return check_; }

  // "Maybe" or "definitely not". A definite no is what lets a lookup skip the
  // table entirely, and it is the only answer the filter is trusted for.
  bool MayContain(Slice user_key) const { return filter_.MayContain(user_key); }

  // The newest version at or below `snapshot`. kNotFound covers both "no such
  // key in this table" and "the newest visible version here is a deletion" --
  // and the CALLER MUST TELL THEM APART, because a deletion in a newer table
  // hides a value in an older one. `*deleted` is how.
  //
  // `*filtered` REPORTS THAT THE BLOOM ANSWERED, and it exists for one reason:
  // without it, a Get that stopped consulting the filter would return exactly
  // the same answers, slower, forever, and no lane could see it. An
  // optimisation whose absence is invisible is an optimisation nobody can
  // assert is present -- so the fact is made observable at the call rather than
  // left to a counter nothing reads.
  // `*found_seq` REPORTS THE SEQUENCE OF THE VERSION RETURNED, and it exists
  // for the same kind of reason as `*filtered`: a caller has to compare it
  // against the newest RANGE TOMBSTONE covering the key, and the tombstone may
  // live in a different store entirely. Without the sequence the caller would
  // have a value and no way to know whether something above it hid it.
  Status Get(Slice user_key, SeqNum snapshot, std::string* value, bool* deleted,
             bool* filtered, SeqNum* found_seq = nullptr) const;

  // A cursor over internal keys, in table order. MAKES NO Env CALL.
  class Iter final : public InternalIter {
   public:
    explicit Iter(const Table* t) : t_(t) {}
    bool Valid() const override { return loaded_ && block_ < t_->blocks_.size(); }
    void SeekToFirst() override;
    void SeekToLast() override;
    // Positions at the first entry >= `target` in the internal order.
    void Seek(Slice target) override;
    void Next() override;
    void Prev() override;
    Slice key() const override;
    Slice value() const override;

   private:
    void LoadBlock(std::size_t i);
    const Table* t_;
    std::size_t block_ = 0;
    std::size_t entry_ = 0;
    std::vector<BlockEntry> entries_;
    bool loaded_ = false;
  };

 private:
  Table() = default;

  struct BlockRef {
    BlockHandle handle;
    Slice last_key;  // into image_
  };

  std::string image_;
  // Slices INTO `image_`, which this object owns and never reallocates after
  // Open. The lifetime is the table's, which is what the shared_ptr every
  // reader holds is for.
  std::vector<RangeTombstone> tombstones_;
  std::vector<BlockRef> blocks_;
  FilterReader filter_;
  TableCheck check_;
  uint64_t number_ = 0;
};

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_TABLE_H_
