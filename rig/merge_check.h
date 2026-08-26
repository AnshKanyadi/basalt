// RIFT_ORACLE -- registered in ORACLES.txt.
//
// DOES THE MERGE COME OUT IN THE RIGHT ORDER WITH THE RIGHT VALUES?
//
// A DIFFERENT QUESTION FROM THE DROP ADJUDICATOR'S, and the reason it needs its
// own instrument is GF-12 one level up: `AdjudicateDrops` is CORRECT ABOUT WHAT
// IT CHECKS AND SILENT ABOUT WHAT A READER ASSUMES IT COVERS. It works over
// SETS of (user_key, seq), so it is blind to ordering entirely and blind to
// values --
//
//   A MERGE THAT EMITTED EVERY REQUIRED ENTRY, IN REVERSE ORDER, WITH EVERY
//   VALUE SHIFTED BY ONE POSITION, WOULD SATISFY ALL THREE OF ITS DIRECTIONS.
//
// -- which is stated concretely because that is specific enough that nobody can
// talk themselves out of it.
//
// ---------------------------------------------------------------------------
// COMPLEMENTARY, NOT REDUNDANT. Written down because someone will later notice
// the overlap and propose deleting one:
//
//   THIS           runs only where the harness knows BOTH the input and the
//                  output files -- a compaction in isolation -- and sees ORDER,
//                  VALUES and DROPS.
//   AdjudicateDrops runs against ANY durable image, including one produced by a
//                  crash MID-COMPACTION, and sees DROPS only.
//
// Delete the second and every crash schedule loses its drop verdict. Delete the
// first and a merge can reverse its output undetected.
//
// ---------------------------------------------------------------------------
// IT LANDS BEFORE THE MERGE, fifth use of the ordering, and here the reason is
// sharper than usual: the merge is the thing whose output ORDER is in question,
// so a checker written afterwards would take its notion of the right order from
// the code that produced it.
//
// WHAT IT MAY TOUCH: artifacts only, per B3-D2a. It is handed bytes and parses
// them with pure decoders.
// WHAT IT MAY CONCLUDE FROM: the harness's own merge of those inputs. B3-D2b
// permits reading the artifact and NOT deriving the expectation from the
// engine -- and here the expectation is derived from the INPUT bytes, which is
// the one place that is sound, because the inputs are not the thing under test.
// The output is.
#ifndef RIFT_RIG_MERGE_CHECK_H_
#define RIFT_RIG_MERGE_CHECK_H_

#include <cstdint>
#include <string>
#include <vector>

#include "run_outcome.h"
#include "version_model.h"

namespace rift {
namespace rig {

struct MergeVerdict {
  RunOutcome outcome = RunOutcome::kContractPass;
  std::string why;

  // EVERY COUNT IS ASSERTED BY A TEST OR DELETED.
  std::size_t input_entries = 0;   // the merge's DERIVED BOUND (GF-13)
  std::size_t output_entries = 0;
  std::size_t expected_entries = 0;

  bool ok() const { return outcome == RunOutcome::kContractPass; }
};

// `inputs` and `output` are raw SSTable images, as the harness holds them.
//
// The expectation is built by MERGING THE INPUTS HERE -- a plain sorted merge in
// the internal order, then filtered by the drop claim against `model` -- and the
// output must equal it EXACTLY: same entries, same order, same values.
MergeVerdict AdjudicateMerge(const VersionModel& model,
                             const std::vector<std::string>& inputs,
                             const std::string& output);

// The derived bound of B3-D7a, exposed so the merge itself can assert against
// the same number the checker uses. GF-13: a bound derived from another
// instrument's measurement cannot be raised without contradicting it.
std::size_t InputEntryCount(const std::vector<std::string>& inputs);

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_MERGE_CHECK_H_
