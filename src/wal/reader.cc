#include "reader.h"

#include <cstring>

#include "check.h"

namespace rift {
namespace wal {

const char* ScanOutcomeName(ScanOutcome outcome) {
  switch (outcome) {  // NO default: arm
    case ScanOutcome::kCleanEnd:           return "clean-end";
    case ScanOutcome::kTornTail:           return "torn-tail";
    case ScanOutcome::kInteriorCorruption: return "interior-corruption";
  }
  RIFT_UNREACHABLE("ScanOutcome holds a value no enumerator names");
}

namespace {

struct Fragment {
  uint32_t crc = 0;
  uint16_t length = 0;
  FragmentType type = FragmentType::kInvalid;
  Slice payload;
};

// Why a header read failed, in the caller's words. Empty means it succeeded.
const char* ReadFragment(Slice image, uint64_t offset, Fragment* out) {
  const uint64_t size = image.size();
  const uint64_t block_off = offset % kBlockBytes;
  const uint64_t left_in_block = kBlockBytes - block_off;

  if (size - offset < kHeaderBytes) return "header truncated by EOF";

  const char* p = image.data() + offset;
  out->crc = static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
             (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
             (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
             (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
  out->length = static_cast<uint16_t>(
      static_cast<uint32_t>(static_cast<unsigned char>(p[4])) |
      (static_cast<uint32_t>(static_cast<unsigned char>(p[5])) << 8));
  const uint8_t t = static_cast<uint8_t>(p[6]);

  // Type 0 is reserved-invalid, so a zero-filled header is rejected BEFORE its
  // CRC is considered. Together with length >= 1 this is what makes a run of
  // zeros -- padding, or a hole past the written extent -- unmistakable for a
  // record, which section 5.4's false-positive analysis rests on.
  if (t == 0 || t > static_cast<uint8_t>(FragmentType::kLast)) {
    return "fragment type is invalid (a zero-filled or garbage header)";
  }
  out->type = static_cast<FragmentType>(t);
  if (out->length == 0) return "fragment length is zero; the format reserves >= 1";
  if (kHeaderBytes + out->length > left_in_block) {
    return "fragment length runs past its block";
  }
  if (size - offset - kHeaderBytes < out->length) return "payload truncated by EOF";

  // THE CRC COVERS length || type || payload. See FragmentCrc in format.h for
  // the divergence from LevelDB and why reverting it silently weakens the
  // torn-tail rule. Reader and writer call the same helper, so there is one
  // definition of the covered range and not two that can drift.
  const Slice payload(p + kHeaderBytes, out->length);
  if (FragmentCrc(out->length, out->type, payload) != out->crc) {
    return "fragment CRC mismatch";
  }

  out->payload = Slice(p + kHeaderBytes, out->length);
  return nullptr;
}

// Is `offset` a structurally valid START of a record whose sequence is above
// the last committed group's? The resync predicate, and every clause of it is
// load-bearing.
bool IsResyncCandidate(Slice image, uint64_t offset, SeqNum committed) {
  Fragment f;
  if (ReadFragment(image, offset, &f) != nullptr) return false;
  // FULL or FIRST only. Accepting a bare MIDDLE would let garbage masquerade as
  // interior corruption and MANUFACTURE a refused open -- an availability bug
  // produced by a safety rule.
  if (f.type != FragmentType::kFull && f.type != FragmentType::kFirst) return false;
  RecordKind kind;
  SeqNum seq;
  if (!PeekKindAndSeq(f.payload, &kind, &seq)) return false;
  // A MANIFEST EDIT COUNTS WITHOUT A SEQUENCE TEST, and without it the
  // manifest's torn-tail rule would be unsound in a way nothing would report.
  //
  // The `seq > committed` test exists so that a stale batch left over from an
  // earlier file cannot masquerade as valid data after a corruption point. A
  // manifest edit has no sequence at all, and a manifest GROUP_END carries
  // zero, so under the WAL's predicate NOTHING in a manifest can ever be
  // recognised as structurally valid -- which would classify every interior
  // corruption in a manifest as a torn tail and SILENTLY DISCARD committed
  // state. Section 5.4's whole discriminator would be answered "no" by
  // construction.
  //
  // It costs the WAL nothing: no WAL contains a manifest edit, so one appearing
  // after a corruption point is genuine interior corruption and refusing the
  // open is the right answer there too.
  if (kind == RecordKind::kManifestEdit) return true;
  if (kind != RecordKind::kBatch && kind != RecordKind::kGroupEnd) return false;
  return seq > committed;
}

bool LegalTransition(bool inside, FragmentType t) {
  switch (t) {  // NO default: arm
    case FragmentType::kInvalid: return false;
    case FragmentType::kFull:    return !inside;
    case FragmentType::kFirst:   return !inside;
    case FragmentType::kMiddle:  return inside;
    case FragmentType::kLast:    return inside;
  }
  RIFT_UNREACHABLE("FragmentType holds a value no enumerator names");
}

}  // namespace

ScanResult ScanLog(Slice image) {
  ScanResult r;
  const uint64_t size = image.size();
  uint64_t offset = 0;
  bool inside = false;
  std::string assembling;
  uint64_t record_start = 0;

  auto commit_record = [&r](RecordKind kind, std::string payload, uint64_t start) {
    LogicalRecord rec;
    rec.kind = kind;
    rec.payload = std::move(payload);
    rec.offset = start;
    r.records.push_back(std::move(rec));
    // Groups already closed by a GROUP_END stand. Everything after the last one
    // is the tail, and discarding it is not an error.
    if (kind == RecordKind::kGroupEnd) {
      RecordKind k;
      SeqNum seq;
      if (PeekKindAndSeq(Slice(r.records.back().payload), &k, &seq)) {
        r.committed_count = r.records.size();
        r.last_committed_seq = seq;
      }
    }
  };

  // fail() ends the log at `offset` and decides between a torn tail and
  // interior corruption. `structural` marks the case where TWO structurally
  // valid fragments formed an illegal transition -- no crash produces that, so
  // it is interior corruption directly and does not go through resync.
  auto fail = [&](const char* why, bool structural) {
    r.failure_offset = offset;
    r.failure_block = offset / kBlockBytes;
    r.failure_reason = why;
    if (structural) {
      r.outcome = ScanOutcome::kInteriorCorruption;
      r.resync_offset = offset;
      return;
    }
    // Resync: advance to the next block boundary and ask whether anything
    // structurally valid lives there. Damage is bounded to one block, and a
    // block boundary is the one alignment always known to be a legal fragment
    // start, which is what makes this bounded rather than a search.
    uint64_t b = ((offset / kBlockBytes) + 1) * kBlockBytes;
    for (; b < size; b += kBlockBytes) {
      if (IsResyncCandidate(image, b, r.last_committed_seq)) {
        r.outcome = ScanOutcome::kInteriorCorruption;
        r.resync_offset = b;
        return;
      }
    }
    r.outcome = ScanOutcome::kTornTail;
  };

  while (offset < size) {
    const uint64_t block_off = offset % kBlockBytes;
    const uint64_t left_in_block = kBlockBytes - block_off;
    if (left_in_block < kMinFragmentBytes) {
      offset += left_in_block;  // padding; the next fragment starts in the next block
      continue;
    }

    Fragment f;
    const char* why = ReadFragment(image, offset, &f);
    if (why != nullptr) {
      fail(why, /*structural=*/false);
      return r;
    }
    if (!LegalTransition(inside, f.type)) {
      // Both fragments are structurally valid and their order is impossible.
      // No crash produces it: prefix truncation cannot create a valid FIRST
      // after a valid FIRST. It is a writer bug or corruption that landed on a
      // fragment boundary, and either way truncation would be unsafe.
      fail("illegal fragment transition", /*structural=*/true);
      return r;
    }

    switch (f.type) {  // NO default: arm
      case FragmentType::kInvalid:
        RIFT_UNREACHABLE("ReadFragment already rejected type 0");
      case FragmentType::kFull: {
        RecordKind kind;
        SeqNum seq;
        if (!PeekKindAndSeq(f.payload, &kind, &seq)) {
          fail("record kind is invalid", /*structural=*/false);
          return r;
        }
        commit_record(kind, f.payload.ToString(), offset);
        break;
      }
      case FragmentType::kFirst:
        record_start = offset;
        assembling.assign(f.payload.data(), f.payload.size());
        inside = true;
        break;
      case FragmentType::kMiddle:
        assembling.append(f.payload.data(), f.payload.size());
        break;
      case FragmentType::kLast: {
        assembling.append(f.payload.data(), f.payload.size());
        RecordKind kind;
        SeqNum seq;
        if (!PeekKindAndSeq(Slice(assembling), &kind, &seq)) {
          fail("record kind is invalid", /*structural=*/false);
          return r;
        }
        commit_record(kind, assembling, record_start);
        assembling.clear();
        inside = false;
        break;
      }
    }
    offset += kHeaderBytes + f.length;
  }

  if (inside) {
    // FIRST (MIDDLE...) then EOF: prefix truncation, and nothing can follow.
    offset = size;
    fail("incomplete multi-fragment record at EOF", /*structural=*/false);
    return r;
  }
  r.outcome = ScanOutcome::kCleanEnd;
  return r;
}

}  // namespace wal
}  // namespace rift
