#include "table_check.h"

#include <vector>

#include "check.h"
#include "internal_key.h"

namespace rift {
namespace sst {

const char* TableFaultName(TableFault fault) {
  switch (fault) {  // NO default: arm
    case TableFault::kNone:                  return "none";
    case TableFault::kTooSmall:              return "image too small";
    case TableFault::kBadMagic:              return "not an SSTable";
    case TableFault::kBadFooterChecksum:     return "footer checksum";
    case TableFault::kUnknownFormatVersion:  return "unknown format version";
    case TableFault::kHandleOutOfRange:      return "block handle out of range";
    case TableFault::kBadBlockChecksum:      return "block checksum";
    case TableFault::kMalformedBlock:        return "malformed block";
    case TableFault::kNotAnInternalKey:      return "key too short to carry a tag";
    case TableFault::kEntriesNotAscending:   return "entries not ascending";
    case TableFault::kIndexNotAscending:     return "index not ascending";
    case TableFault::kIndexSeparatorMismatch:return "index separator is not the block's last key";
    case TableFault::kBadRangeBlock:            return "malformed range-tombstone block";
    case TableFault::kTombstoneOutsideTheTableBounds:
      return "a range tombstone the table's own bounds do not admit";
    case TableFault::kEmptyTable:            return "table has no data blocks";
  }
  RIFT_UNREACHABLE("TableFault holds a value no enumerator names");
}

namespace {

TableCheck Fail(TableFault f, uint64_t offset, const std::string& why) {
  TableCheck c;
  c.fault = f;
  c.offset = offset;
  c.why = why;
  return c;
}

}  // namespace

TableCheck ValidateTable(Slice image) {
  Footer footer;
  std::string why;
  if (image.size() < kFooterBytes) {
    return Fail(TableFault::kTooSmall, 0, "image is smaller than a footer");
  }
  const uint64_t footer_at = image.size() - kFooterBytes;
  if (!DecodeFooter(image, &footer, &why)) {
    const TableFault f = (why.find("magic") != std::string::npos)
                             ? TableFault::kBadMagic
                             : TableFault::kBadFooterChecksum;
    return Fail(f, footer_at, why);
  }
  if (footer.format_version != kFormatVersion) {
    return Fail(TableFault::kUnknownFormatVersion, footer_at,
                "format version " + std::to_string(footer.format_version));
  }

  // EVERY HANDLE IS RANGE-CHECKED BEFORE IT IS FOLLOWED. The footer is the one
  // structure read without trusting anything else in the file; from here on,
  // nothing is dereferenced until its bounds have been proven against a checksum
  // that already passed.
  auto in_range = [&](const BlockHandle& h) {
    return h.offset <= footer_at && h.size <= footer_at - h.offset;
  };
  if (!in_range(footer.index)) {
    return Fail(TableFault::kHandleOutOfRange, footer_at,
                "index handle runs past the footer");
  }
  if (footer.filter.size != 0 && !in_range(footer.filter)) {
    return Fail(TableFault::kHandleOutOfRange, footer_at,
                "filter handle runs past the footer");
  }

  std::vector<BlockEntry> index_entries;
  std::vector<uint32_t> index_restarts;
  const Slice index_block(image.data() + footer.index.offset, footer.index.size);
  if (!ParseBlock(index_block, &index_entries, &index_restarts, &why)) {
    const TableFault f = (why.find("checksum") != std::string::npos)
                             ? TableFault::kBadBlockChecksum
                             : TableFault::kMalformedBlock;
    return Fail(f, footer.index.offset, "index block: " + why);
  }
  if (index_entries.empty()) {
    return Fail(TableFault::kEmptyTable, footer.index.offset,
                "the index names no data blocks");
  }

  TableCheck ok;
  Slice previous_index_key;
  bool have_previous_index_key = false;

  for (const BlockEntry& ie : index_entries) {
    // An index key is an internal key too -- it is a copy of some block's last
    // one -- so it is length-checked before it is compared. Comparing first
    // would abort on a corrupt file, and a corrupt file must produce a VERDICT.
    if (ie.key.size() < kTagBytes) {
      return Fail(TableFault::kNotAnInternalKey, footer.index.offset + ie.offset,
                  "index key is " + std::to_string(ie.key.size()) + " bytes");
    }
    if (have_previous_index_key &&
        CompareInternalKey(ie.key, previous_index_key) <= 0) {
      return Fail(TableFault::kIndexNotAscending,
                  footer.index.offset + ie.offset,
                  "index key does not exceed the one before it");
    }
    previous_index_key = ie.key;
    have_previous_index_key = true;

    BlockHandle h;
    if (!DecodeHandle(ie.value, &h)) {
      return Fail(TableFault::kMalformedBlock, footer.index.offset + ie.offset,
                  "index value is not a block handle");
    }
    if (!in_range(h)) {
      return Fail(TableFault::kHandleOutOfRange, footer.index.offset + ie.offset,
                  "data block handle runs past the footer");
    }

    std::vector<BlockEntry> entries;
    std::vector<uint32_t> restarts;
    const Slice data_block(image.data() + h.offset, h.size);
    if (!ParseBlock(data_block, &entries, &restarts, &why)) {
      const TableFault f = (why.find("checksum") != std::string::npos)
                               ? TableFault::kBadBlockChecksum
                               : TableFault::kMalformedBlock;
      return Fail(f, h.offset, "data block: " + why);
    }
    if (entries.empty()) {
      return Fail(TableFault::kMalformedBlock, h.offset, "data block is empty");
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].key.size() < kTagBytes) {
        return Fail(TableFault::kNotAnInternalKey, h.offset + entries[i].offset,
                    "key is " + std::to_string(entries[i].key.size()) + " bytes");
      }
      // STRICTLY ascending IN THE INTERNAL KEY ORDER -- user key ascending, tag
      // DESCENDING. A bytewise comparison orders two versions of one user key
      // the wrong way round and would accept a table whose readers find the
      // OLDEST visible version at every snapshot seek.
      if (i > 0 && CompareInternalKey(entries[i].key, entries[i - 1].key) <= 0) {
        return Fail(TableFault::kEntriesNotAscending, h.offset + entries[i].offset,
                    "entry key does not exceed the one before it");
      }
    }
    // B2-D2, MADE CHECKABLE. The separator is the block's LAST KEY exactly, so
    // there is something in the table to compare it against. A shortened
    // separator -- LevelDB's -- would leave this assertion impossible to write,
    // which is the argument for the decision rather than a consequence of it.
    if (entries.back().key != ie.key) {
      return Fail(TableFault::kIndexSeparatorMismatch,
                  footer.index.offset + ie.offset,
                  "index names a key that is not the block's last");
    }
    if (ok.data_blocks == 0) {
      ok.smallest_key.assign(entries.front().key.data(), entries.front().key.size());
    }
    ok.largest_key.assign(entries.back().key.data(), entries.back().key.size());
    for (const BlockEntry& e : entries) {
      const uint64_t seq = SeqOfTag(ExtractTag(e.key));
      if (seq > ok.largest_seq) ok.largest_seq = seq;
    }
    ok.data_blocks++;
    ok.entries += entries.size();
  }

  // ------------------------------------------------------- the range block
  //
  // ITS SIZE IS DERIVED, NOT STORED: the reserve held eight bytes and a handle
  // is twelve, so only the offset is written and the block must be LAST. See
  // table_format.h. A wrong derivation cannot produce a wrong answer -- the
  // block's crc32c covers exactly the bytes the derived size names, so any
  // disagreement fails the checksum. B1's CRC-covering-the-length property,
  // reused one format over: corruption the reader REJECTS rather than believes.
  if (footer.range_offset != 0) {
    if (footer.range_offset >= footer_at) {
      return Fail(TableFault::kHandleOutOfRange, footer_at,
                  "range block offset runs past the footer");
    }
    // It must not overlap what is already spoken for. Every other handle was
    // range-checked before it was followed; this one is checked against them.
    if (footer.range_offset < footer.index.offset + footer.index.size ||
        (footer.filter.size != 0 &&
         footer.range_offset < footer.filter.offset + footer.filter.size)) {
      return Fail(TableFault::kHandleOutOfRange, footer_at,
                  "range block overlaps the index or the filter");
    }
    const uint64_t range_size = footer_at - footer.range_offset;
    std::vector<RangeTombstone> tombstones;
    const Slice range_block(image.data() + footer.range_offset,
                            static_cast<std::size_t>(range_size));
    const RangeCheck rc = ParseRangeBlock(range_block, &tombstones);
    if (!rc.ok()) {
      return Fail(TableFault::kBadRangeBlock, footer.range_offset + rc.offset,
                  std::string("range block: ") + RangeFaultName(rc.fault) + " (" +
                      rc.why + ")");
    }
    ok.range_tombstones = tombstones.size();

    // SECTION 6.1's REFUSAL THAT IS NOT ABOUT THE BLOCK. The manifest records
    // this table's bounds and COMPACTION CHOOSES ITS INPUTS BY THEM, so a
    // tombstone the bounds do not admit is one no compaction will read -- and
    // clause 2 of the drop claim then permits dropping a deletion while
    // something it masked survives elsewhere. Input selection is a correctness
    // concern, so the bounds that drive it are one too.
    const Slice lo = ExtractUserKey(Slice(ok.smallest_key));
    const Slice hi = ExtractUserKey(Slice(ok.largest_key));
    for (const RangeTombstone& t : tombstones) {
      if (t.start.compare(lo) < 0 || t.end.compare(hi) > 0) {
        return Fail(TableFault::kTombstoneOutsideTheTableBounds,
                    footer.range_offset + t.offset,
                    "tombstone range lies outside the table's own key bounds");
      }
      if (t.seq() > ok.largest_seq) ok.largest_seq = t.seq();
    }
  }
  return ok;
}

}  // namespace sst
}  // namespace rift
