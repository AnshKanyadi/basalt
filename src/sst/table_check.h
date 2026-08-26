// THE SSTABLE CLASSIFIER. It lands before the writer, on the B1 ruling.
//
//   The torn-tail rule and fragment-chain legality are the freeze surface, so
//   their gates are induced before the writer is trusted.
//
// B2 freezes a second format and inherits that ordering unchanged. This file
// and its tests exist with NO WRITER IN THE TREE, driven entirely from
// hand-built byte images -- so the writer's output is later checked against
// rules already seen to reject every illegal shape, rather than against a
// decoder written to agree with it.
//
// THE ORDERING RULE IS NOT MEMCMP. Entries are INTERNAL keys and their order is
// user key ascending, tag DESCENDING -- see internal_key.h, which exists
// because this file was first drafted comparing raw bytes and the fixtures that
// would have caught it did not exist yet.
//
// WHAT IT REJECTS, and each row is a shape a fixture builds by hand:
//
//   an image too small to hold a footer
//   a footer whose magic is not RIFTSST            -- a foreign file
//   a footer whose own checksum fails              -- a damaged one
//   an unknown format version
//   an index or filter handle running past the footer
//   a block whose checksum fails
//   a block declaring zero restarts, or a restart array that does not fit,
//     or a restart offset past the entries
//   an entry whose key or value runs past the block
//   a key too short to carry a tag                 -- not an internal key
//   entries not strictly ascending within a block, IN THE INTERNAL KEY ORDER
//   index entries not strictly ascending, in the same order
//   AN INDEX ENTRY THAT DOES NOT EQUAL THE LAST KEY OF THE BLOCK IT NAMES
//
// The last one is B2-D2's whole justification made checkable. It is only
// possible because the separator is the block's exact last key: a shortened
// separator would have nothing in the table to be compared against.
//
// The filter block is RANGE-CHECKED here and not PARSED: its own classifier is
// bloom.h's FilterReader::Parse, which lands with the filter at B2.1. A filter
// whose declared length disagrees with its block is refused there.
#ifndef RIFT_SST_TABLE_CHECK_H_
#define RIFT_SST_TABLE_CHECK_H_

#include <cstdint>
#include <string>

#include "range_tombstone.h"
#include "table_format.h"
#include "slice.h"

namespace rift {
namespace sst {

// CLOSED. -Werror=switch, no default: arm -- a new fault must be named before
// it can be returned, which is what stops "something was wrong" from becoming a
// reportable outcome.
enum class TableFault : uint8_t {
  kNone,
  kTooSmall,
  kBadMagic,
  kBadFooterChecksum,
  kUnknownFormatVersion,
  kHandleOutOfRange,
  kBadBlockChecksum,
  kMalformedBlock,
  kNotAnInternalKey,
  kEntriesNotAscending,
  kIndexNotAscending,
  kIndexSeparatorMismatch,
  kEmptyTable,
  kBadRangeBlock,
  kTombstoneOutsideTheTableBounds,
};
const char* TableFaultName(TableFault fault);

struct TableCheck {
  TableFault fault = TableFault::kNone;
  // Where, at full precision. A refused table that cannot say where is one
  // nobody can act on -- section 5.4's rule, one format over.
  uint64_t offset = 0;
  std::string why;

  bool ok() const { return fault == TableFault::kNone; }
  // Filled in on success, so a caller that validates does not have to parse
  // again to find out what it validated.
  //
  // `largest_seq` is the number D4 section 5.1 point 2 holds the manifest to:
  // it is a fact about the bytes on disk, derived by the classifier the
  // manifest cannot influence, and an Open fails if the manifest disagrees with
  // it. THE MAXIMUM OVER ALL ENTRIES, not the sequence of the largest key --
  // the two are unrelated, and BM46 is the mutant that says so.
  uint64_t data_blocks = 0;
  uint64_t entries = 0;
  // Range tombstones the table carries. Zero for every table written before
  // B3.5, which is what `range_offset == 0` decodes to.
  uint64_t range_tombstones = 0;
  std::string smallest_key;
  std::string largest_key;
  uint64_t largest_seq = 0;
};

// Validates a whole table image. Pure: bytes in, verdict out, no Env, no
// engine. That purity is why these gates need no rig.
TableCheck ValidateTable(Slice image);

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_TABLE_CHECK_H_
