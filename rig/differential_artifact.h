// THE DIFFERENTIAL ARTIFACT: the C++ half of a format frozen in
// docs/FORMAT-differential.md, and specified there BEFORE either decoder
// existed.
//
// ---------------------------------------------------------------------------
// THIS FILE IS ONE OF TWO INDEPENDENT DECODERS. The other is Go, in
// `engine/differential`, and it is NOT a binding over this one.
//
//   A SHARED DECODER MAKES WRITER AND READER AGREE BY CONSTRUCTION, AND THE
//   SINGLE BUG A FROZEN FORMAT EXISTS TO CATCH IS A DISAGREEMENT ABOUT WHAT THE
//   BYTES MEAN. BYTE-FOR-BYTE AGREEMENT IS THE THING UNDER TEST, SO IT CANNOT
//   BE THE THING ASSUMED.
//
// It costs a second implementation, maintained. What makes it affordable is
// that THE FIXTURES ARE THE SHARED ARTIFACT RATHER THAN THE CODE: both decoders
// are checked against the same committed bytes in `seeds/differential/format/`,
// hand-built from the document and never produced by either encoder.
//
// ---------------------------------------------------------------------------
// THE CORPUS PROMISE, IN ITS STRICT FORM (B4-Q3):
//
//   A STORED ARTIFACT MUST REPRODUCE ITS FINDING, NOT MERELY REPLAY.
//
// which is why `VERDICT` is a section rather than something the judge recomputes
// and keeps to itself. A replay that reaches a different verdict FAILS.
//
// ---------------------------------------------------------------------------
// NOT AN ORACLE. It encodes and it classifies; it reaches no verdict about the
// engine. The VERDICT it carries was reached elsewhere and is data here.
#ifndef RIFT_RIG_DIFFERENTIAL_ARTIFACT_H_
#define RIFT_RIG_DIFFERENTIAL_ARTIFACT_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "slice.h"

namespace rift {
namespace rig {

inline constexpr char kDiffMagic[8] = {'R', 'I', 'F', 'T', 'D', 'I', 'F', '\0'};
inline constexpr uint32_t kDiffFormatVersion = 1;

// CLOSED, -Werror=switch, no default: arm. A new section must be named before
// it can be encoded, decoded or refused.
enum class DiffSection : uint8_t {
  kProvenance = 1,
  kSubmission = 2,
  kWatermark = 3,
  kRecovered = 4,
  kVerdict = 5,
};
// The five are ALL REQUIRED and appear in ASCENDING KIND ORDER, so two
// artifacts of one run are byte-identical and the corpus can be diffed.
inline constexpr uint8_t kDiffFirstSection = 1;
inline constexpr uint8_t kDiffLastSection = 5;

enum class DiffOpKind : uint8_t {
  kSet = 1,
  kDelete = 2,
  kDeleteRange = 3,
  kSync = 4,
  kSnapshotTake = 5,
  kSnapshotRelease = 6,
};

// The verdict the judge reached. `kUnrun` is the value an artifact carries
// before a judge has seen it -- and it is REFUSED by the classifier, because an
// artifact without a verdict cannot reproduce a finding.
enum class DiffOutcome : uint8_t {
  kUnrun = 0,
  kAgree = 1,
  kRecoveredLess = 2,
  kRecoveredMore = 3,
  kRecoveredNeither = 4,
};
const char* DiffOutcomeName(DiffOutcome o);

struct DiffOp {
  DiffOpKind kind = DiffOpKind::kSet;
  uint64_t seq = 0;
  std::string key;    // SET/DELETE: the key. DELETE_RANGE: the start.
  std::string value;  // SET: the value. DELETE_RANGE: the end.
  // AN EMPTY KEY IS A VALID KEY, so boundedness cannot be carried by emptiness
  // -- db.h divergence 3, surviving into the artifact. Without these,
  // DeleteRange(At(""), At("")) and DeleteRange(Unbounded, Unbounded) are the
  // same bytes, and those are a range that deletes nothing and section 8.2's
  // clear-everything case.
  bool start_bounded = false;
  bool end_bounded = false;
};

struct DiffProvenance {
  std::string engine_commit;
  std::string model_commit;
  uint64_t seed = 0;
  uint64_t flush_bytes = 0;
  uint64_t wal_buffer_bytes = 0;
  uint64_t max_record_bytes = 0;
};

struct DiffArtifact {
  DiffProvenance provenance;
  std::vector<DiffOp> submission;
  uint64_t watermark = 0;
  // std::map: STRICTLY ASCENDING by key is a format rule, and a map is what
  // makes the encoder unable to break it.
  std::map<std::string, std::string> recovered;
  DiffOutcome outcome = DiffOutcome::kUnrun;
  std::string why;
};

// CLOSED. Every refusal in FORMAT-differential.md §3 has an enumerator, so a
// refusal that cannot be named cannot be returned.
enum class DiffFault : uint8_t {
  kNone,
  kTooSmall,
  kBadMagic,
  kBadTrailingMagic,
  kBadChecksum,
  kUnknownFormatVersion,
  kSectionRunsPastTheEnd,
  kUnknownSectionKind,
  kDuplicateSection,
  kMissingSection,
  kSectionsNotAscending,
  kTrailingBytes,
  kEmptySubmission,
  kRecoveredNotAscending,
  kUnknownOutcome,
  kMissingCommit,
  kDirtyCommit,
  kSequencesNotMonotone,
  kMalformedSection,
};
const char* DiffFaultName(DiffFault f);

struct DiffCheck {
  DiffFault fault = DiffFault::kNone;
  uint64_t offset = 0;
  std::string why;
  bool ok() const { return fault == DiffFault::kNone; }
};

// Encodes an artifact. RIFT_CHECKs the invariants the format requires of a
// WRITER -- a non-empty submission, both commits present and clean, a verdict
// that is not kUnrun -- because a writer that emits a file its own classifier
// refuses is a bug in this process, not a damaged file.
std::string EncodeDiffArtifact(const DiffArtifact& a);

// Parses AND CHECKS, from bytes alone. Pure: no Env, no engine, no writer.
DiffCheck ParseDiffArtifact(Slice image, DiffArtifact* out);

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_DIFFERENTIAL_ARTIFACT_H_
