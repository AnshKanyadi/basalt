// THE DIFFERENTIAL ARTIFACT: the C++ half of a format frozen in
// docs/FORMAT-differential.md, and specified there BEFORE either decoder
// existed.
//
// ---------------------------------------------------------------------------
// THIS FILE IS ONE OF TWO INDEPENDENT DECODERS. The other lives with the
// reference model, in a separate process, and it is NOT a binding over this
// one.
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

// The verdict the judge reached.
//
// `kUnrun` IS A LEGAL VALUE AND THE CLASSIFIER ACCEPTS IT, which is a
// correction to this file's first version and worth stating rather than
// quietly fixing.
//
// The original refused it, reasoning that "an artifact without a verdict cannot
// reproduce a finding." That is true of a CORPUS ENTRY and false of a FILE: the
// driver cannot reach a verdict, because reaching one requires the reference
// model. So the artifact necessarily exists unjudged for the length of
// its journey between the two processes, and a format that refused that state
// would have refused every file the driver can write.
//
// THE CORPUS PROMISE ATTACHES TO A JUDGED ARTIFACT, NOT TO THE FORMAT. It is
// enforced by `RequireJudged` at the corpus gate rather than by the parser --
// which is the same shape as B3's bounds rule (6.1a): the requirement was about
// what the MANIFEST records, not about what any file may contain.
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
  // DRIVER-SIDE ONLY, and NOT IN THE FORMAT. It marks where the author decided
  // a new batch begins; the ARTIFACT expresses the same fact by every op in a
  // batch carrying one sequence, which is what a batch is in this engine.
  //
  // Keeping it out of the format is deliberate: a field recording something the
  // sequences already say is a second source of truth about one fact, and the
  // two would drift.
  bool batch_head = false;
};

// THE REGIMES THE DIFFERENTIAL RIG RUNS. Closed, -Werror=switch, no default.
enum class DiffRegime : uint8_t {
  kDefault = 1,     // shipped caps: no flush in a short run
  kFlush = 2,       // a low flush threshold, so tables exist
  kCompact = 3,     // low enough, and long enough, to cross the L0 trigger
};
// ONE PLACE PRODUCES THE STRING, and the artifact carries what this function
// returns rather than a literal written beside the caps. See the header note on
// `DiffProvenance::regime`.
const char* DiffRegimeName(DiffRegime r);

struct DiffProvenance {
  std::string engine_commit;
  std::string model_commit;
  // WHICH REGIME PRODUCED THIS, AND IT IS A REQUIRED FIELD RATHER THAN A
  // DERIVABLE ONE.
  //
  // The caps identify the CONFIGURATION and not the SWEEP: two regimes can
  // share caps and differ in workload -- `flush` and `compact` do exactly that
  // -- so "which regime" is not recoverable from the numbers beside it.
  //
  //   PROVENANCE THAT IS DERIVABLE-IN-PRINCIPLE FROM OTHER FIELDS IS PROVENANCE
  //   NOBODY DERIVES.
  //
  // Track A paid for that three times -- `power-config: a3`, the A6 banner, and
  // the single-label opt-out -- each a label that stopped describing its
  // subject while remaining, in principle, reconstructible.
  //
  // AND IT IS DERIVED FROM THE REGIME'S OWN NAME rather than written beside it,
  // which is the fix that ended the banner problem: the writer passes a
  // `DiffRegime` and the string comes from `DiffRegimeName`, so a renamed
  // regime cannot leave a stale label in an artifact.
  std::string regime;
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
  // NOT IN THE FORMAT, and deliberately: it is a diagnostic for the driver's
  // own use, not a field an artifact carries. A reopen that fails means there
  // is nothing to judge, and the driver reports it rather than emitting an
  // artifact that says the engine recovered nothing.
  std::string reopen_error;
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
  kMissingRegime,
  kUnjudged,
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
// Accepts an UNJUDGED artifact; see `DiffOutcome`.
DiffCheck ParseDiffArtifact(Slice image, DiffArtifact* out);

// THE CORPUS GATE. A file under `seeds/differential/` must have been judged, or
// it cannot reproduce a finding -- B4-Q3's strict form. Separate from the
// parser because it is a rule about what the CORPUS holds, not about what the
// format admits.
DiffCheck RequireJudged(const DiffArtifact& a);

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_DIFFERENTIAL_ARTIFACT_H_
