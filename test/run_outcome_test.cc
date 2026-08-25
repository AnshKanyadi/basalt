#include "run_outcome.h"

#include <cstddef>
#include <iterator>
#include <set>
#include <string>

#include <gtest/gtest.h>

namespace rift {
namespace rig {
namespace {

constexpr RunOutcome kAllOutcomes[] = {
    RunOutcome::kContractPass,        RunOutcome::kContractViolation,
    RunOutcome::kCharacterizationOnly, RunOutcome::kInconclusive,
    RunOutcome::kVoid,
};
constexpr std::size_t kExpectedOutcomeCount = 5;

TEST(RunOutcome, EveryKindIsNamedAndNamesAreDistinct) {
  ASSERT_EQ(std::size(kAllOutcomes), kExpectedOutcomeCount);
  std::set<std::string> names;
  for (RunOutcome o : kAllOutcomes) names.insert(RunOutcomeName(o));
  EXPECT_EQ(names.size(), kExpectedOutcomeCount);
}

// THE LEDGER TEST. This is what BM13 has to get past.
//
// Exactly one kind counts as evidence for the recovery contract, and it is the
// one in which both assertions were made and both held. The others are not
// degrees of pass: a violation is a bug, an inconclusive check did not finish,
// a void was adjudicated legitimate by the harness, and a characterization run
// SUSPENDED assertion (ii) -- so the contract it would be cited for was never
// under test in that run.
TEST(RunOutcome, ExactlyOneKindCountsAsRecoveryEvidence) {
  int counted = 0;
  for (RunOutcome o : kAllOutcomes) {
    if (CountsAsRecoveryEvidence(o)) ++counted;
  }
  EXPECT_EQ(counted, 1) << "more than one kind of run is being banked as evidence";
  EXPECT_TRUE(CountsAsRecoveryEvidence(RunOutcome::kContractPass));
}

TEST(RunOutcome, CharacterizationIsNotEvidence) {
  EXPECT_FALSE(CountsAsRecoveryEvidence(RunOutcome::kCharacterizationOnly))
      << "a run with an exactness-suspending injector enabled did not test the "
         "recovery contract, so it cannot be cited for it -- in any column, "
         "ledger or README sentence";
}

TEST(RunOutcome, ViolationInconclusiveAndVoidAreNotEvidence) {
  EXPECT_FALSE(CountsAsRecoveryEvidence(RunOutcome::kContractViolation));
  EXPECT_FALSE(CountsAsRecoveryEvidence(RunOutcome::kInconclusive));
  EXPECT_FALSE(CountsAsRecoveryEvidence(RunOutcome::kVoid));
}

// The heading has to be unmisreadable by someone skimming a table, which is a
// different requirement from the policy function being right. Amendment A4
// made "inconclusive" a first-class column for exactly this reason; sections
// 7.5 and 7.6 add two more that are also not passes.
TEST(RunOutcome, NonEvidenceColumnsSayNotEvidenceInTheHeading) {
  for (RunOutcome o : kAllOutcomes) {
    const std::string column = RunOutcomeLedgerColumn(o);
    if (CountsAsRecoveryEvidence(o)) continue;
    if (o == RunOutcome::kContractViolation) continue;  // "violation" is louder still
    EXPECT_NE(column.find("not evidence"), std::string::npos)
        << "column heading '" << column << "' can be skimmed as a pass";
  }
}

}  // namespace
}  // namespace rig
}  // namespace rift
