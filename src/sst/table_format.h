// THE SSTABLE FORMAT. B2-D1/B2-D2, approved, and this is the frozen surface.
//
// Fixed-width little-endian, no varints, NO TIMESTAMPS anywhere (ruling 2), and
// NO FLOATS on any path that reaches these bytes (ruling 5).
//
// ---------------------------------------------------------------------------
// B2-D1: ENTRIES ARE SELF-DESCRIBING. NO PREFIX COMPRESSION IN v1.
//
// LevelDB shares a prefix between adjacent entries and restarts every k keys.
// It is smaller. It also makes an entry's key A FUNCTION OF THE ENTRY BEFORE
// IT, so a corrupted length inside a block yields a key that PARSES PERFECTLY
// and is wrong.
//
// THAT IS THE CRC-COVERING-THE-LENGTH DECISION ONE LAYER UP, and the connection
// is recorded here because anyone reverting either one should have to answer
// the same question: DOES CORRUPTION PRODUCE SOMETHING THE READER REJECTS, OR
// SOMETHING IT ACCEPTS? A length outside the CRC yields a wrong-sized payload
// whose failure offset is unknown. A shared prefix yields a wrong key. Neither
// cheap option loses detection -- both convert a DETECTED fault into an
// ACCEPTED one, which is strictly worse than an outage and is what section 5.4
// rejected candidate (b) for.
//
// Amendment A6 decides the rest: the simplest correct thing wins v1 and the
// smaller thing is a recorded upgrade path. The threshold that reopens it is a
// number, not a mood -- B5's standalone numbers showing SSTable size or block
// cache pressure is a bottleneck, with the cost of full keys attributed by
// measurement rather than inferred.
//
// ---------------------------------------------------------------------------
// B2-D2: THE INDEX SEPARATOR IS THE BLOCK'S LAST KEY, EXACTLY.
//
// LevelDB computes the shortest string separating block N from block N+1. It is
// smaller, and it PUTS A KEY IN THE INDEX THAT IS NOT IN THE TABLE. B4 defines
// correct as byte-identical to engine/model, and a synthesized key is a byte no
// model produces; it also leaves the index unvalidatable, because there is
// nothing in the table to validate it against.
//
// Exact last keys make every index entry CHECKABLE: it must equal the last key
// of the block it names, and the classifier asserts that from the bytes alone.
//
// COST, STATED SO NOBODY LATER READS IT AS AN OVERSIGHT: THE INDEX IS LARGER.
// Our separator grows with key LENGTH where LevelDB's grows with key
// DISTINGUISHABILITY, and on A5's MVCC encoding -- where every internal key
// shares a user-key prefix -- that difference is real. It is being paid
// deliberately. It is not an omission and it is not a TODO.
//
// ---------------------------------------------------------------------------
//   file  = data blocks || filter block || index block || footer
//
//   block = entries || restart_array || restart_count:u32 || crc32c:u32
//   entry = key_len:u32 || key || value_len:u32 || value      (INTERNAL keys)
//   restart_array = restart_count * u32, offsets from the block's start
//
//   THE INDEX IS ITSELF A BLOCK. Its entries are (last_key -> 12-byte handle),
//   so there is ONE block decoder and ONE checksum path rather than two that
//   can drift -- section 7.5's one-mechanism rule applied to a decoder.
//   handle = offset:u64 || size:u32
//
//   footer, FIXED 48 BYTES, read from EOF:
//     filter_offset:u64   filter_size:u32
//     index_offset:u64    index_size:u32
//     format_version:u32
//     reserved:[8]u8 = 0
//     magic:[8]u8 = "RIFTSST\0"
//     crc32c:u32          covering the 44 bytes above it
//
// The footer is FIXED WIDTH on purpose: it is the one thing a classifier can
// read without trusting anything else in the file, so every other offset is
// reached through a checksum that has already been verified.
//
// `reserved` is zero and never written otherwise, for the same reason
// DELETE_RANGE is a reserved op kind from day one: eight bytes now are free, a
// format version bump at B3 is not.
// NAMED table_format.h AND NOT format.h. Every source directory under src/ is on
// the include path, so two headers with one basename are resolved by SEARCH
// ORDER rather than by name: a quoted include finds the includer's own
// directory first and everything else finds src/wal/format.h. That worked here
// by accident and would stop working the moment a file outside src/sst wanted
// this header. A latent trap that depends on where a file lives is exactly the
// kind this project exists to remove, so no basename is duplicated on the
// include path.
#ifndef RIFT_SST_TABLE_FORMAT_H_
#define RIFT_SST_TABLE_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "slice.h"

namespace rift {
namespace sst {

inline constexpr std::size_t kFooterBytes = 48;
inline constexpr std::size_t kFooterCrcCovers = 44;
inline constexpr std::size_t kBlockTrailerBytes = 4;   // crc32c
inline constexpr std::size_t kHandleBytes = 12;        // offset:u64 size:u32
inline constexpr char kMagic[8] = {'R', 'I', 'F', 'T', 'S', 'S', 'T', '\0'};
inline constexpr uint32_t kFormatVersion = 1;

// One restart every kRestartInterval entries. Restarts exist ONLY as a
// binary-search index into the block: with self-describing entries they are not
// needed to decode, which is precisely what makes B2-D1 safe.
inline constexpr uint32_t kRestartInterval = 16;

struct BlockHandle {
  uint64_t offset = 0;
  uint32_t size = 0;
};

struct Footer {
  BlockHandle filter;
  BlockHandle index;
  uint32_t format_version = 0;
};

void EncodeHandle(const BlockHandle& h, std::string* out);
bool DecodeHandle(Slice s, BlockHandle* out);

void EncodeFooter(const Footer& f, std::string* out);
// Reads the footer from the END of `image`. Returns false and sets `why` if the
// image is too small, the magic is wrong, or the footer's own CRC fails.
bool DecodeFooter(Slice image, Footer* out, std::string* why);

// Builds one block. Exposed because the classifier's fixtures build blocks by
// hand: a fixture that used a different encoder than the reader validates would
// be testing the fixture.
class BlockBuilder {
 public:
  void Add(Slice key, Slice value);
  // Appends the restart array, the count and the checksum, and returns the
  // block. The builder is spent afterwards.
  std::string Finish();
  std::size_t entries() const { return entries_; }
  std::size_t size_estimate() const { return buf_.size(); }

 private:
  std::string buf_;
  std::vector<uint32_t> restarts_;
  std::size_t entries_ = 0;
  uint32_t since_restart_ = 0;
};

struct BlockEntry {
  Slice key;
  Slice value;
  uint64_t offset = 0;  // from the block's start, for error reporting
};

// Parses and CHECKS a block: the trailing checksum, the restart count and every
// restart offset, and that entries do not run past the block. Ordering is NOT
// checked here -- the table classifier checks it, because "ascending" means
// different things for a data block and an index block only in what a failure
// implies, and one parser with two callers is better than two parsers.
bool ParseBlock(Slice block, std::vector<BlockEntry>* entries,
                std::vector<uint32_t>* restarts, std::string* why);

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_TABLE_FORMAT_H_
