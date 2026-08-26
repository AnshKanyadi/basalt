#include "range_tombstone.h"

#include "check.h"
#include "table_format.h"

namespace rift {
namespace sst {

const char* RangeFaultName(RangeFault fault) {
  switch (fault) {  // NO default: arm
    case RangeFault::kNone:                 return "none";
    case RangeFault::kMalformedBlock:       return "malformed range block";
    case RangeFault::kEmptyOrInvertedRange: return "range end is not above its start";
    case RangeFault::kNotAscending:         return "tombstones not ascending by start";
    case RangeFault::kDuplicate:            return "two tombstones with one start and one tag";
    case RangeFault::kNotADeleteRangeTag:   return "tag is not a DELETE_RANGE";
    case RangeFault::kUnboundedEndWithBytes:
      return "an unbounded end that carries end bytes";
  }
  RIFT_UNREACHABLE("RangeFault holds a value no enumerator names");
}

namespace {

void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
uint32_t GetU32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}
uint64_t GetU64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

RangeCheck Fail(RangeFault f, uint64_t offset, const std::string& why) {
  RangeCheck c;
  c.fault = f;
  c.offset = offset;
  c.why = why;
  return c;
}

}  // namespace

void EncodeRangeTombstone(Slice start, Slice end, uint64_t tag, std::string* out) {
  RIFT_CHECK(end.size() != kUnboundedEndLen);  // unreachable, and stated anyway
  PutU32(out, static_cast<uint32_t>(start.size()));
  out->append(start.data(), start.size());
  PutU32(out, static_cast<uint32_t>(end.size()));
  out->append(end.data(), end.size());
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((tag >> (8 * i)) & 0xff));
}

void EncodeUnboundedRangeTombstone(Slice start, uint64_t tag, std::string* out) {
  PutU32(out, static_cast<uint32_t>(start.size()));
  out->append(start.data(), start.size());
  PutU32(out, kUnboundedEndLen);
  // AND NO END BYTES. The parser refuses a record that claims the sentinel and
  // carries them, so there is exactly one encoding of "no upper bound".
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((tag >> (8 * i)) & 0xff));
}

RangeCheck ParseRangeBlock(Slice block, std::vector<RangeTombstone>* out) {
  out->clear();
  // THE SAME BLOCK DECODER THE DATA BLOCKS USE. ParseBlock verifies the
  // trailing checksum, the restart count and every restart offset, and bounds
  // every length before reading it -- so none of that is re-implemented here,
  // and none of it can drift.
  std::vector<BlockEntry> entries;
  std::vector<uint32_t> restarts;
  std::string why;
  if (!ParseBlock(block, &entries, &restarts, &why)) {
    return Fail(RangeFault::kMalformedBlock, 0, why);
  }

  // A tombstone is one BlockEntry's key: start||end||tag, decoded here. The
  // entry's VALUE is unused and must be empty -- a range tombstone carries no
  // value, and a non-empty one is a writer meaning something this format does
  // not express.
  Slice previous_start;
  uint64_t previous_tag = 0;
  bool have_previous = false;

  for (const BlockEntry& e : entries) {
    const char* p = e.key.data();
    std::size_t left = e.key.size();
    if (left < 4) return Fail(RangeFault::kMalformedBlock, e.offset, "truncated start length");
    const uint32_t slen = GetU32(p);
    p += 4; left -= 4;
    if (left < slen) return Fail(RangeFault::kMalformedBlock, e.offset, "start runs past the entry");
    RangeTombstone t;
    t.start = Slice(p, slen);
    p += slen; left -= slen;
    if (left < 4) return Fail(RangeFault::kMalformedBlock, e.offset, "truncated end length");
    const uint32_t elen = GetU32(p);
    p += 4; left -= 4;
    if (elen == kUnboundedEndLen) {
      // B3-Q4's sentinel. The record carries NO end bytes, and one that claims
      // otherwise is refused -- so "no upper bound" has exactly one encoding and
      // there is no second one to disagree with it.
      t.end_unbounded = true;
      if (left != 8) {
        return Fail(RangeFault::kUnboundedEndWithBytes, e.offset,
                    "end length is the unbounded sentinel and " +
                        std::to_string(left - 8) + " end bytes follow");
      }
    } else {
      if (left < elen) return Fail(RangeFault::kMalformedBlock, e.offset, "end runs past the entry");
      t.end = Slice(p, elen);
      p += elen; left -= elen;
      if (left != 8) return Fail(RangeFault::kMalformedBlock, e.offset, "entry is not exactly a tombstone");
    }
    t.tag = GetU64(p);
    t.offset = e.offset;
    if (!e.value.empty()) {
      return Fail(RangeFault::kMalformedBlock, e.offset, "a range tombstone carries no value");
    }

    // [start, end), HALF-OPEN. An empty or inverted range covers nothing and no
    // writer can mean it; accepting it makes "covers nothing" and "covers
    // everything below start" indistinguishable at the reader.
    // A RULE ABOUT FINITE ENDS, and the sentinel is not an exception to it --
    // it is a different case, in which there is no end to compare. Written this
    // way round because the next reader meets the guard before the sentinel.
    if (!t.end_unbounded && t.end.compare(t.start) <= 0) {
      return Fail(RangeFault::kEmptyOrInvertedRange, e.offset,
                  "end is not above start");
    }
    // A POINT VERSION IN THE RANGE BLOCK is data that exists and is unreachable:
    // the point path will never look here for it.
    if (TypeOfTag(t.tag) != ValueType::kDeletion) {
      return Fail(RangeFault::kNotADeleteRangeTag, e.offset,
                  "tag type is not a deletion");
    }
    if (have_previous) {
      const int c = t.start.compare(previous_start);
      // The block is BINARY-SEARCHED to find the tombstones covering a key. An
      // unsorted block does not fail -- it returns the WRONG ANSWER, which is a
      // resurrected key or a wrongly hidden one.
      if (c < 0) {
        return Fail(RangeFault::kNotAscending, e.offset,
                    "start is below the one before it");
      }
      if (c == 0 && t.tag == previous_tag) {
        return Fail(RangeFault::kDuplicate, e.offset,
                    "same start and same tag as the one before it");
      }
    }
    previous_start = t.start;
    previous_tag = t.tag;
    have_previous = true;
    out->push_back(t);
  }

  RangeCheck ok;
  ok.count = out->size();
  return ok;
}

}  // namespace sst
}  // namespace rift
