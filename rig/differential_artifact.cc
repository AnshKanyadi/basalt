#include "differential_artifact.h"

#include <cstring>

#include "basalt/check.h"
#include "crc32c.h"

namespace basalt {
namespace rig {

const char* DiffOutcomeName(DiffOutcome o) {
  switch (o) {  // NO default: arm
    case DiffOutcome::kUnrun:            return "unrun";
    case DiffOutcome::kAgree:            return "agree";
    case DiffOutcome::kRecoveredLess:    return "recovered less than promised";
    case DiffOutcome::kRecoveredMore:    return "recovered more than promised";
    case DiffOutcome::kRecoveredNeither: return "recovered a state at no watermark";
  }
  BASALT_UNREACHABLE("DiffOutcome holds a value no enumerator names");
}

const char* DiffRegimeName(DiffRegime r) {
  switch (r) {  // NO default: arm
    case DiffRegime::kDefault: return "default";
    case DiffRegime::kFlush:   return "flush";
    case DiffRegime::kCompact: return "compact";
  }
  BASALT_UNREACHABLE("DiffRegime holds a value no enumerator names");
}

const char* DiffFaultName(DiffFault f) {
  switch (f) {  // NO default: arm
    case DiffFault::kNone:                  return "none";
    case DiffFault::kTooSmall:              return "smaller than a header and a footer";
    case DiffFault::kBadMagic:              return "not a differential artifact";
    case DiffFault::kBadTrailingMagic:      return "incomplete: the file ends before its footer";
    case DiffFault::kBadChecksum:           return "checksum mismatch";
    case DiffFault::kUnknownFormatVersion:  return "format version this build cannot place";
    case DiffFault::kSectionRunsPastTheEnd: return "a section runs past the footer";
    case DiffFault::kUnknownSectionKind:    return "unknown section kind";
    case DiffFault::kDuplicateSection:      return "two sections of one kind";
    case DiffFault::kMissingSection:        return "a required section is absent";
    case DiffFault::kSectionsNotAscending:  return "sections are not in ascending kind order";
    case DiffFault::kTrailingBytes:         return "bytes after the footer";
    case DiffFault::kEmptySubmission:       return "an artifact of no operations";
    case DiffFault::kRecoveredNotAscending: return "recovered keys are not strictly ascending";
    case DiffFault::kUnknownOutcome:        return "verdict names no enumerator";
    case DiffFault::kMissingCommit:         return "provenance names no commit";
    case DiffFault::kDirtyCommit:           return "provenance names an uncommitted tree";
    case DiffFault::kSequencesNotMonotone:  return "submission sequences are not non-decreasing";
    case DiffFault::kMalformedSection:      return "a section's payload is malformed";
    case DiffFault::kMissingRegime:         return "provenance names no regime";
    case DiffFault::kUnjudged:              return "artifact has not been judged";
  }
  BASALT_UNREACHABLE("DiffFault holds a value no enumerator names");
}

namespace {

constexpr std::size_t kHeaderBytes = 8 + 4 + 4;   // magic, version, count
constexpr std::size_t kFooterBytes = 8 + 4;       // magic, crc32c

void PutU8(std::string* o, uint8_t v) { o->push_back(static_cast<char>(v)); }
void PutU32(std::string* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::string* o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutStr(std::string* o, const std::string& s) {
  PutU32(o, static_cast<uint32_t>(s.size()));
  o->append(s);
}

// A cursor that bounds every read before it makes it. Nothing in this file
// dereferences a length that has not been through it.
class Cursor {
 public:
  Cursor(const char* p, std::size_t n) : p_(p), left_(n) {}
  bool U8(uint8_t* v) {
    if (left_ < 1) return false;
    *v = static_cast<uint8_t>(static_cast<unsigned char>(*p_));
    ++p_; --left_;
    return true;
  }
  bool U32(uint32_t* v) {
    if (left_ < 4) return false;
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) {
      r |= static_cast<uint32_t>(static_cast<unsigned char>(p_[i])) << (8 * i);
    }
    *v = r; p_ += 4; left_ -= 4;
    return true;
  }
  bool U64(uint64_t* v) {
    if (left_ < 8) return false;
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) {
      r |= static_cast<uint64_t>(static_cast<unsigned char>(p_[i])) << (8 * i);
    }
    *v = r; p_ += 8; left_ -= 8;
    return true;
  }
  bool Str(std::string* v) {
    uint32_t n = 0;
    if (!U32(&n)) return false;
    if (left_ < n) return false;
    v->assign(p_, n);
    p_ += n; left_ -= n;
    return true;
  }
  bool Done() const { return left_ == 0; }

 private:
  const char* p_;
  std::size_t left_;
};

DiffCheck Fail(DiffFault f, uint64_t offset, const std::string& why) {
  DiffCheck c;
  c.fault = f;
  c.offset = offset;
  c.why = why;
  return c;
}

}  // namespace

std::string EncodeDiffArtifact(const DiffArtifact& a) {
  // A WRITER THAT EMITS A FILE ITS OWN CLASSIFIER REFUSES IS A BUG IN THIS
  // PROCESS, not a damaged file -- so these are BASALT_CHECKs and not a Status.
  BASALT_CHECK(!a.submission.empty());
  BASALT_CHECK(!a.provenance.engine_commit.empty());
  BASALT_CHECK(!a.provenance.model_commit.empty());
  BASALT_CHECK(!a.provenance.regime.empty());

  std::string body;
  const auto section = [&body](DiffSection kind, const std::string& payload) {
    PutU8(&body, static_cast<uint8_t>(kind));
    PutU32(&body, static_cast<uint32_t>(payload.size()));
    body.append(payload);
  };

  // ASCENDING KIND ORDER, and it is not a convention here -- it is emitted in
  // this order because the classifier refuses any other, so two artifacts of
  // one run are byte-identical.
  {
    std::string p;
    PutStr(&p, a.provenance.engine_commit);
    PutStr(&p, a.provenance.model_commit);
    PutStr(&p, a.provenance.regime);
    PutU64(&p, a.provenance.seed);
    PutU64(&p, a.provenance.flush_bytes);
    PutU64(&p, a.provenance.wal_buffer_bytes);
    PutU64(&p, a.provenance.max_record_bytes);
    section(DiffSection::kProvenance, p);
  }
  {
    std::string p;
    PutU32(&p, static_cast<uint32_t>(a.submission.size()));
    for (const DiffOp& op : a.submission) {
      PutU8(&p, static_cast<uint8_t>(op.kind));
      PutU64(&p, op.seq);
      PutStr(&p, op.key);
      PutStr(&p, op.value);
      PutU8(&p, static_cast<uint8_t>((op.start_bounded ? 1u : 0u) |
                                     (op.end_bounded ? 2u : 0u)));
    }
    section(DiffSection::kSubmission, p);
  }
  {
    std::string p;
    PutU64(&p, a.watermark);
    section(DiffSection::kWatermark, p);
  }
  {
    std::string p;
    PutU32(&p, static_cast<uint32_t>(a.recovered.size()));
    for (const auto& kv : a.recovered) {  // std::map: ascending by construction
      PutStr(&p, kv.first);
      PutStr(&p, kv.second);
    }
    section(DiffSection::kRecovered, p);
  }
  {
    std::string p;
    PutU8(&p, static_cast<uint8_t>(a.outcome));
    PutStr(&p, a.why);
    section(DiffSection::kVerdict, p);
  }

  std::string out;
  out.append(kDiffMagic, sizeof(kDiffMagic));
  PutU32(&out, kDiffFormatVersion);
  PutU32(&out, 5);  // section_count: all five, always
  out.append(body);
  out.append(kDiffMagic, sizeof(kDiffMagic));
  PutU32(&out, wal::Crc32c(out.data(), out.size()));
  return out;
}

DiffCheck ParseDiffArtifact(Slice image, DiffArtifact* out) {
  *out = DiffArtifact();
  if (image.size() < kHeaderBytes + kFooterBytes) {
    return Fail(DiffFault::kTooSmall, 0, "image is " + std::to_string(image.size()) + " bytes");
  }
  const char* p = image.data();
  if (std::memcmp(p, kDiffMagic, sizeof(kDiffMagic)) != 0) {
    return Fail(DiffFault::kBadMagic, 0, "header magic is not RIFTDIF");
  }
  // THE TRAILING MAGIC IS CHECKED BEFORE ANYTHING IS FOLLOWED, so a TRUNCATED
  // file is reported as incomplete rather than discovered later as a section
  // that overruns -- which would report "malformed" for a file that is not.
  const std::size_t footer_at = image.size() - kFooterBytes;
  if (std::memcmp(p + footer_at, kDiffMagic, sizeof(kDiffMagic)) != 0) {
    return Fail(DiffFault::kBadTrailingMagic, footer_at, "the file ends before its footer");
  }
  const std::size_t crc_at = image.size() - 4;
  uint32_t stored = 0;
  for (int i = 0; i < 4; ++i) {
    stored |= static_cast<uint32_t>(static_cast<unsigned char>(p[crc_at + i])) << (8 * i);
  }
  if (wal::Crc32c(p, crc_at) != stored) {
    return Fail(DiffFault::kBadChecksum, crc_at, "checksum mismatch");
  }

  Cursor head(p + sizeof(kDiffMagic), kHeaderBytes - sizeof(kDiffMagic));
  uint32_t version = 0;
  uint32_t count = 0;
  BASALT_CHECK(head.U32(&version) && head.U32(&count));
  if (version != kDiffFormatVersion) {
    return Fail(DiffFault::kUnknownFormatVersion, 8,
                "format version " + std::to_string(version));
  }

  std::size_t at = kHeaderBytes;
  bool seen[kDiffLastSection + 1] = {false};
  uint8_t previous_kind = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 5 > footer_at) {
      return Fail(DiffFault::kSectionRunsPastTheEnd, at, "section header runs past the footer");
    }
    const uint8_t kind = static_cast<uint8_t>(static_cast<unsigned char>(p[at]));
    uint32_t len = 0;
    for (int b = 0; b < 4; ++b) {
      len |= static_cast<uint32_t>(static_cast<unsigned char>(p[at + 1 + b])) << (8 * b);
    }
    const std::size_t payload_at = at + 5;
    if (payload_at + len > footer_at) {
      return Fail(DiffFault::kSectionRunsPastTheEnd, at,
                  "section of " + std::to_string(len) + " bytes runs past the footer");
    }
    // THE RANGE CHECK COMES FIRST, because `kind` came off disk and can hold a
    // value no enumerator names. A switch over a closed enum cannot express
    // that case.
    if (kind < kDiffFirstSection || kind > kDiffLastSection) {
      // REFUSED, NEVER SKIPPED: an artifact whose unknown section is ignored
      // has lost the thing it was meant to carry and reports success.
      return Fail(DiffFault::kUnknownSectionKind, at, "section kind " + std::to_string(kind));
    }
    if (seen[kind]) {
      return Fail(DiffFault::kDuplicateSection, at, "second section of kind " + std::to_string(kind));
    }
    if (kind <= previous_kind) {
      return Fail(DiffFault::kSectionsNotAscending, at,
                  "kind " + std::to_string(kind) + " follows " + std::to_string(previous_kind));
    }
    seen[kind] = true;
    previous_kind = kind;

    Cursor c(p + payload_at, len);
    switch (static_cast<DiffSection>(kind)) {  // NO default: arm
      case DiffSection::kProvenance: {
        DiffProvenance& v = out->provenance;
        if (!c.Str(&v.engine_commit) || !c.Str(&v.model_commit) ||
            !c.Str(&v.regime) || !c.U64(&v.seed) ||
            !c.U64(&v.flush_bytes) || !c.U64(&v.wal_buffer_bytes) ||
            !c.U64(&v.max_record_bytes) || !c.Done()) {
          return Fail(DiffFault::kMalformedSection, payload_at, "provenance");
        }
        if (v.engine_commit.empty() || v.model_commit.empty()) {
          return Fail(DiffFault::kMissingCommit, payload_at,
                      "an artifact naming no commit cannot be reproduced");
        }
        const auto dirty = [](const std::string& s) {
          return s.size() >= 6 && s.compare(s.size() - 6, 6, "-dirty") == 0;
        };
        if (dirty(v.engine_commit) || dirty(v.model_commit)) {
          return Fail(DiffFault::kDirtyCommit, payload_at,
                      "a run at an uncommitted tree cannot be reproduced");
        }
        if (v.regime.empty()) {
          return Fail(DiffFault::kMissingRegime, payload_at,
                      "the caps identify the configuration and not the sweep");
        }
        break;
      }
      case DiffSection::kSubmission: {
        uint32_t ops = 0;
        if (!c.U32(&ops)) return Fail(DiffFault::kMalformedSection, payload_at, "submission count");
        if (ops == 0) {
          return Fail(DiffFault::kEmptySubmission, payload_at, "no operations");
        }
        uint64_t previous_seq = 0;
        for (uint32_t j = 0; j < ops; ++j) {
          DiffOp op;
          uint8_t k = 0;
          uint8_t flags = 0;
          if (!c.U8(&k) || !c.U64(&op.seq) || !c.Str(&op.key) || !c.Str(&op.value) ||
              !c.U8(&flags)) {
            return Fail(DiffFault::kMalformedSection, payload_at,
                        "operation " + std::to_string(j));
          }
          if (k < 1 || k > 6) {
            return Fail(DiffFault::kMalformedSection, payload_at,
                        "operation kind " + std::to_string(k));
          }
          op.kind = static_cast<DiffOpKind>(k);
          op.start_bounded = (flags & 1u) != 0;
          op.end_bounded = (flags & 2u) != 0;
          // THE MODEL REPLAYS IN THIS ORDER, so a log it cannot replay in order
          // is a log the judge cannot use -- and finding that out in the judge
          // would report it as a divergence in the engine.
          //
          // ZERO IS EXEMPT, AND THE FIRST VERSION WAS NOT. `Sync`,
          // `SnapshotTake` and `SnapshotRelease` consume no sequence and carry
          // 0 by the format's own definition -- so a check phrased as "never
          // decreases" refuses every legal artifact containing a Sync, which is
          // all of them. The GF-14 half of the covering test found it: the
          // refusal direction passed, and asserting the ACCEPTING direction is
          // what showed the rule was too strict.
          if (op.seq != 0 && op.seq < previous_seq) {
            return Fail(DiffFault::kSequencesNotMonotone, payload_at,
                        "sequence " + std::to_string(op.seq) + " follows " +
                            std::to_string(previous_seq));
          }
          if (op.seq != 0) previous_seq = op.seq;
          out->submission.push_back(std::move(op));
        }
        if (!c.Done()) return Fail(DiffFault::kMalformedSection, payload_at, "trailing bytes in submission");
        break;
      }
      case DiffSection::kWatermark: {
        if (!c.U64(&out->watermark) || !c.Done()) {
          return Fail(DiffFault::kMalformedSection, payload_at, "watermark");
        }
        break;
      }
      case DiffSection::kRecovered: {
        uint32_t entries = 0;
        if (!c.U32(&entries)) return Fail(DiffFault::kMalformedSection, payload_at, "recovered count");
        std::string previous_key;
        bool have_previous = false;
        for (uint32_t j = 0; j < entries; ++j) {
          std::string k, v;
          if (!c.Str(&k) || !c.Str(&v)) {
            return Fail(DiffFault::kMalformedSection, payload_at, "recovered entry");
          }
          if (have_previous && !(previous_key < k)) {
            return Fail(DiffFault::kRecoveredNotAscending, payload_at,
                        "key does not exceed the one before it");
          }
          previous_key = k;
          have_previous = true;
          out->recovered.emplace(std::move(k), std::move(v));
        }
        if (!c.Done()) return Fail(DiffFault::kMalformedSection, payload_at, "trailing bytes in recovered");
        break;
      }
      case DiffSection::kVerdict: {
        uint8_t o = 0;
        if (!c.U8(&o) || !c.Str(&out->why) || !c.Done()) {
          return Fail(DiffFault::kMalformedSection, payload_at, "verdict");
        }
        if (o > static_cast<uint8_t>(DiffOutcome::kRecoveredNeither)) {
          return Fail(DiffFault::kUnknownOutcome, payload_at, "outcome " + std::to_string(o));
        }
        out->outcome = static_cast<DiffOutcome>(o);
        break;
      }
    }
    at = payload_at + len;
  }

  if (at != footer_at) {
    return Fail(DiffFault::kTrailingBytes, at, "bytes between the last section and the footer");
  }
  for (uint8_t k = kDiffFirstSection; k <= kDiffLastSection; ++k) {
    if (!seen[k]) {
      return Fail(DiffFault::kMissingSection, 0, "section kind " + std::to_string(k));
    }
  }
  return DiffCheck();
}

DiffCheck RequireJudged(const DiffArtifact& a) {
  if (a.outcome == DiffOutcome::kUnrun) {
    return Fail(DiffFault::kUnjudged, 0,
                "an artifact without a verdict cannot reproduce a finding");
  }
  return DiffCheck();
}

}  // namespace rig
}  // namespace basalt
