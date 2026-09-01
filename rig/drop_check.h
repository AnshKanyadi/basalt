// BASALT_ORACLE -- registered in ORACLES.txt.
//
// THE DROP ADJUDICATOR. B3-D2, and it lands BEFORE any compaction exists, which
// is the observer-before-the-observed rule at its most important point in this
// phase: THE ADJUDICATOR IS THE ONLY THING THAT CAN DISTINGUISH A POLICY THAT
// DROPS CORRECTLY FROM ONE THAT DROPS TOO MUCH, AND AN ADJUDICATOR WRITTEN AFTER
// THE POLICY WILL AGREE WITH IT.
//
// ---------------------------------------------------------------------------
// WHY A STATE COMPARISON CANNOT DO THIS JOB.
//
// B2's recovery equivalence compares state at a watermark; the sweep compares
// state after a kill. Both read at ONE sequence. A compaction that honours the
// drop claim at the current sequence and violates it at every snapshot passes
// both, and passes every other check in this tree, because the versions it
// wrongly dropped were never visible at the sequence anyone looked at.
//
// So this is a comparison over DROPS, not over survivors, and it runs in THREE
// directions:
//
//   kept >= required     nothing a reader can reach was dropped
//   dropped <= permitted no tombstone was dropped over something it masked
//   survived <= submitted  THE ENGINE MAY NOT HOLD A VERSION NOBODY WROTE
//
// An engine that drops NOTHING is as wrong as one that drops too much -- it is
// a compaction that does not compact -- and every state comparison in the tree
// calls it correct.
//
// THE THIRD DIRECTION IS THE ANSWER TO THE SHARED-PARSER ALIASING, and it is
// deliberate rather than lucky. The dangerous aliasing is a reader that reports
// a record the bytes do not contain: a real drop then looks survived and the
// verdict is a FALSE PASS. Directions one and two cannot see it -- both ask
// whether something is MISSING. The third asks whether something is PRESENT
// THAT WAS NEVER WRITTEN, which is exactly what a fabricating reader produces,
// and it is why READER-shows-a-dropped-record dies here rather than by luck
// somewhere downstream.
//
// ---------------------------------------------------------------------------
// WHAT IT MAY TOUCH, AND WHAT IT MAY CONCLUDE FROM. Two different questions.
//
// TOUCH: artifacts only, per B3-D2a's marks -- an artifact header declares
// nothing taking an `Env*` and nothing taking a snapshot. It is handed a
// DurableImage, the harness's own path-to-bytes map, and parses it with pure
// decoders. No Env, no engine object, no I/O, and no question ever put to the
// engine.
//
// CONCLUDE FROM: the harness's submission log, via VersionModel, and NEVER the
// bytes it just parsed. B3-D2b. Deriving the expectation from the engine's own
// artifacts would hide a record from BOTH sides of the comparison and make the
// shared-parser aliasing TOTAL rather than one-sided.
#ifndef BASALT_RIG_DROP_CHECK_H_
#define BASALT_RIG_DROP_CHECK_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "run_outcome.h"
#include "version_model.h"

namespace basalt {
namespace rig {

// path -> bytes, as the harness holds it. Declared here rather than included
// from TestEnv so this header depends on nothing that could grow an opinion.
using ImageBytes = std::map<std::string, std::string>;

struct DropVerdict {
  RunOutcome outcome = RunOutcome::kContractPass;
  std::string why;

  // EVERY COUNT HERE IS ASSERTED BY A TEST OR DELETED. A count nobody asserts
  // is decoration that looks like evidence.
  std::size_t survived = 0;         // versions found in tables or WALs
  std::size_t required_total = 0;   // versions no reader may lose
  std::size_t dropped = 0;          // submitted, and now nowhere durable
  std::size_t phantom = 0;          // found durable, and never submitted
  std::size_t tables_read = 0;
  std::size_t wals_read = 0;

  bool ok() const { return outcome == RunOutcome::kContractPass; }
};

// Adjudicates one durable image against the harness's own model.
//
// `dir` is the directory prefix the image's paths carry, so the checker can
// tell a table from a WAL by NAME -- which is a fact about the path, not a
// question put to the engine.
DropVerdict AdjudicateDrops(const VersionModel& model, const ImageBytes& image,
                            const std::string& dir);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_DROP_CHECK_H_
