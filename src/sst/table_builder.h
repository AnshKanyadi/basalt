// THE SSTABLE WRITER. It lands AFTER the classifier, on B2-D6.
//
// Every rule this writer's output is checked against was written down, and seen
// to reject the shape it forbids, before this file existed. That ordering is
// what stops a format from being defined by its writer -- a decoder written to
// agree with an encoder validates nothing, and the two agree most confidently
// exactly where they are both wrong.
#ifndef RIFT_SST_TABLE_BUILDER_H_
#define RIFT_SST_TABLE_BUILDER_H_

#include <cstdint>
#include <string>

#include "bloom.h"
#include "env.h"
#include "table_format.h"
#include "internal_key.h"
#include "range_tombstone.h"
#include "slice.h"
#include "status.h"

namespace rift {
namespace sst {

// THE BLOCK SIZE, AND ITS DERIVATION, AT THE DEFINITION SITE -- section 8.4's
// rule, the same one that governs kMaxRecordBytes.
//
// A block is the unit of READ and the unit of CHECKSUM: a point lookup pays for
// one whole block whether it wanted one entry or forty. 4 KiB is a page, so a
// block read is one page fault rather than two, and it bounds the cost of a
// checksum verification per lookup.
//
// The index cost is the other half, and B2-D2 made it larger on purpose: with
// exact last keys as separators, the index carries one FULL internal key per
// block. At 4 KiB blocks and 30-byte keys that is about 50 index bytes per 4096
// data bytes -- about 1.2%. Halving the block size would double it.
//
// THE MEASUREMENT THAT WOULD MOVE IT: B5's standalone numbers, showing block
// size attributed by profile rather than inferred. Absent that number, 4 KiB.
inline constexpr std::size_t kDataBlockBytes = 4096;

class TableBuilder {
 public:
  // Does not own the file and does not close it: the flush path owns the
  // ordering of Sync and Close, because the ORDER of those calls is the whole
  // crash-consistency claim (B2-D5) and it does not belong inside a writer.
  explicit TableBuilder(WritableFile* file) : file_(file) {}

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // Keys must arrive STRICTLY ASCENDING in the internal order. A caller that
  // breaks that is a bug in THIS PROCESS, not a damaged file, so it is a
  // RIFT_CHECK and not a Status: the memtable iterator is asserted to emit
  // exactly this order (sst_format_test.cc), and if it ever does not, the
  // useful place to stop is here rather than in a table nobody can read.
  void Add(Slice internal_key, Slice value);

  // A RANGE TOMBSTONE, `[start, end)` over USER keys. `tag` is a full internal
  // tag whose ValueType must be a deletion -- the classifier refuses anything
  // else, and it refused it before this method existed.
  //
  // Tombstones must arrive ASCENDING BY START, and no two may share a start and
  // a tag. Same discipline as `Add`, same reason: the block is binary-searched,
  // so an unsorted one does not fail -- it returns the wrong answer.
  //
  // IT WIDENS THE TABLE'S RECORDED BOUNDS, and that is a correctness link
  // rather than bookkeeping. The manifest records those bounds and compaction
  // CHOOSES ITS INPUTS BY THEM (B3-D1 clause 2), so a tombstone reachable only
  // through a table whose bounds do not admit it is a tombstone no read will
  // consult -- which resurrects everything it was supposed to mask.
  void AddRangeTombstone(Slice start, Slice end, uint64_t tag);

  // The same, for a range with NO UPPER BOUND (B3-Q4). It widens `smallest` and
  // deliberately does NOT widen `largest`: there is no finite key to widen it
  // to. The fact travels as `TableCheck::unbounded_end` instead, which says the
  // table's range is `[smallest, infinity)` -- an over-approximation, and
  // over-covering costs a read while under-covering resurrects data.
  void AddUnboundedRangeTombstone(Slice start, uint64_t tag);

  // Writes the last data block, the filter, the index, THE RANGE BLOCK and the
  // footer -- in that order, and the range block's position is load-bearing:
  // its size is derived as `file_size - kFooterBytes - range_offset`, so
  // anything written between it and the footer corrupts the derivation for
  // every reader. Asserted here, not merely stated (`BM85`).
  //
  // Writes the last data block, the filter, the index and the footer. The
  // caller Syncs. RIFT_CHECKs that at least one entry was added: an SSTable
  // with no data is a file the classifier refuses, and a flush with nothing to
  // write must SKIP rather than produce one.
  Status Finish();

  const Status& status() const { return status_; }
  uint64_t file_size() const { return offset_; }
  uint64_t entries() const { return entries_; }

  // For the manifest at B2.3. `largest_seq` is what recovery re-derives from
  // the table's own keys and compares against the manifest's record -- D4
  // section 5.1 point 2, the mechanism that keeps the manifest from being the
  // sole authority for any number.
  Slice smallest() const { return Slice(smallest_); }
  Slice largest() const { return Slice(largest_); }
  SeqNum largest_seq() const { return largest_seq_; }

 private:
  Status Append(Slice bytes);
  void FlushDataBlock();

  WritableFile* file_;
  BlockBuilder data_;
  BlockBuilder index_;
  BlockBuilder range_;
  FilterBuilder filter_;
  Status status_;
  uint64_t offset_ = 0;
  uint64_t entries_ = 0;
  std::size_t block_entries_ = 0;
  std::string last_key_;
  std::string last_range_start_;
  uint64_t last_range_tag_ = 0;
  std::size_t range_count_ = 0;
  bool unbounded_end_ = false;
  std::string smallest_;
  std::string largest_;
  SeqNum largest_seq_ = 0;
  bool finished_ = false;
};

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_TABLE_BUILDER_H_
