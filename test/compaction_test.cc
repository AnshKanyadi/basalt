// COMPACTION, JUDGED BY THE TWO INSTRUMENTS THAT LANDED BEFORE IT.
//
// B3-D7a names them and says why there must be two:
//
//   does it stop?        `inputs_consumed` against the DERIVED bound, asserted
//                        inside the loop. A TERMINATION assertion, and GF-12
//                        says it must not be read as more.
//   is the output right? `AdjudicateMerge`  -- order and values.
//                        `AdjudicateDrops`  -- what survived, over any image.
//
// Nothing here re-derives an expectation from the compaction. The model is the
// harness's submission log (B3-D2b), and the merge adjudicator's expectation
// comes from the INPUT bytes, which are not the thing under test.
#include "compaction.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "drop_check.h"
#include "image_fixture.h"
#include "internal_key.h"
#include "manifest.h"
#include "merge_check.h"
#include "table.h"
#include "table_builder.h"
#include "test_env.h"

namespace rift {
namespace {

using rig::ModelSeq;
using rig::VersionModel;
using testenv::TestEnvironment;

const std::string kDir = "db";

struct Cell {
  std::string key;
  SeqNum seq = 0;
  bool deletion = false;
  std::string value;
};

std::string IKey(const Cell& c) {
  std::string out;
  AppendInternalKey(&out, Slice(c.key),
                    MakeTag(c.seq, c.deletion ? ValueType::kDeletion
                                              : ValueType::kValue));
  return out;
}

// One SSTable image, from cells given in TABLE ORDER -- user key ascending, tag
// descending. A group that is not is a fixture testing TableBuilder's refusal.
std::string TableBytes(const std::vector<Cell>& cells) {
  TestEnvironment t;
  EXPECT_TRUE(t.env()->CreateDir(kDir).ok());
  const std::string path = kDir + "/scratch.sst";
  WritableFilePtr f;
  EXPECT_TRUE(t.env()->NewWritableFile(path, &f).ok());
  sst::TableBuilder b(f.get());
  for (const Cell& c : cells) {
    const std::string k = IKey(c);
    b.Add(Slice(k), Slice(c.value));
  }
  EXPECT_TRUE(b.Finish().ok());
  EXPECT_TRUE(f->Sync().ok());
  EXPECT_TRUE(f->Close().ok());
  return t.ContentNow(path);
}

VersionModel ModelOf(const std::vector<std::vector<Cell>>& inputs,
                     const std::vector<SeqNum>& observable) {
  VersionModel m;
  SeqNum visible = 0;
  for (const auto& group : inputs) {
    for (const Cell& c : group) {
      m.NoteWrite(c.key, c.seq, c.deletion, c.value);
      if (c.seq > visible) visible = c.seq;
    }
  }
  for (SeqNum s : observable) {
    if (s == observable.back()) continue;
    m.NoteSnapshotTaken(s);
  }
  m.NoteVisibleSeq(observable.empty() ? visible : observable.back());
  return m;
}

// The whole output in ONE file, whatever its size. The engine rolls a run of
// bounded files; these tests are about the DROP RULES, and a fixture that also
// rolled would make every assertion below depend on where the roll landed.
class OneFile final : public CompactionSink {
 public:
  explicit OneFile(sst::TableBuilder* b) : b_(b) {}
  Status Add(Slice internal_key, Slice value, bool) override {
    b_->Add(internal_key, value);
    return b_->status();
  }

 private:
  sst::TableBuilder* b_;
};

struct CompactResult {
  std::string output;
  std::vector<std::string> inputs;
  CompactionStats stats;
  uint64_t bound = 0;
  SeqNum pin_seq = 0;
};

// Runs a real compaction over fixture tables, in one environment, and hands
// back the bytes on both sides so the adjudicators can be handed them.
CompactResult Compact(const std::vector<std::vector<Cell>>& groups,
                      const std::vector<SeqNum>& observable,
                      bool bottom_most = true) {
  CompactResult r;
  auto t = std::unique_ptr<TestEnvironment>(new TestEnvironment());
  EXPECT_TRUE(t->env()->CreateDir(kDir).ok());
  std::vector<std::shared_ptr<sst::Table>> tables;
  uint64_t number = 1;
  for (const auto& group : groups) {
    const std::string bytes = TableBytes(group);
    r.inputs.push_back(bytes);
    const std::string path = sst::TablePath(kDir, number);
    WritableFilePtr f;
    EXPECT_TRUE(t->env()->NewWritableFile(path, &f).ok());
    EXPECT_TRUE(f->Append(Slice(bytes)).ok());
    EXPECT_TRUE(f->Sync().ok());
    EXPECT_TRUE(f->Close().ok());
    std::shared_ptr<sst::Table> opened;
    EXPECT_TRUE(sst::Table::Open(t->env(), path, number, &opened).ok());
    tables.push_back(opened);
    ++number;
  }

  MergedIter merge;
  for (const auto& tbl : tables) {
    r.bound += tbl->check().entries;
    if (tbl->check().largest_seq > r.pin_seq) r.pin_seq = tbl->check().largest_seq;
    merge.AddTable(tbl.get());
  }

  const std::string out_path = sst::TablePath(kDir, number);
  WritableFilePtr out;
  EXPECT_TRUE(t->env()->NewWritableFile(out_path, &out).ok());
  sst::TableBuilder b(out.get());
  OneFile sink(&b);
  EXPECT_TRUE(RunCompaction(&merge, observable, bottom_most, r.pin_seq, r.bound,
                            &sink, &r.stats)
                  .ok());
  if (r.stats.emitted > 0) {
    EXPECT_TRUE(b.Finish().ok());
    EXPECT_TRUE(out->Sync().ok());
    EXPECT_TRUE(out->Close().ok());
    r.output = t->ContentNow(out_path);
  }
  return r;
}

// ------------------------------------------------------------ the drop claim

TEST(Compaction, WithNoSnapshotOnlyTheNewestVersionOfEachKeySurvives) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, false, "new"}, {"a", 4, false, "old"}, {"b", 7, false, "b7"}}};
  const CompactResult r = Compact(in, {9});
  EXPECT_EQ(2u, r.stats.emitted);
  EXPECT_EQ(1u, r.stats.dropped);
  const rig::MergeVerdict v = AdjudicateMerge(ModelOf(in, {9}), r.inputs, r.output);
  EXPECT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(2u, v.expected_entries);
}

TEST(Compaction, ALiveSnapshotKeepsTheVersionItCanSee) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, false, "new"}, {"a", 4, false, "old"}}};
  // A snapshot at 5 sees version 4; without it, 4 is unobservable.
  const CompactResult r = Compact(in, {5, 9});
  EXPECT_EQ(2u, r.stats.emitted);
  EXPECT_EQ(0u, r.stats.dropped);
  const rig::MergeVerdict v = AdjudicateMerge(ModelOf(in, {5, 9}), r.inputs, r.output);
  EXPECT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(2u, v.expected_entries);
}

// SECTION 1.2a, THE PHASE'S FIRST RESULT, EXERCISED. A tombstone with nothing
// left to mask is dropped -- the claim that forbade it would have made
// compaction stop terminating in space.
TEST(Compaction, ATombstoneWithNothingLeftToMaskIsDropped) {
  // THE TOMBSTONE IS NOT AT THE TOP SEQUENCE, ON PURPOSE. The watermark pin
  // keeps the highest-sequenced entry whatever the claim says, so a fixture
  // that put the tombstone there would be watching the pin and calling it the
  // drop rule. "b" at 10 carries the sequence instead.
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, true, ""}, {"a", 4, false, "old"}, {"b", 10, false, "b10"}}};
  const CompactResult r = Compact(in, {10});
  // "a" vanishes entirely: the deletion is not required, and what it masked is
  // dropped with it.
  EXPECT_EQ(1u, r.stats.emitted);
  EXPECT_EQ(2u, r.stats.dropped);
  EXPECT_EQ(0u, r.stats.pinned);
  const rig::MergeVerdict v = AdjudicateMerge(ModelOf(in, {10}), r.inputs, r.output);
  EXPECT_TRUE(v.ok()) << v.why;
}

// CLAUSE 2. A deletion dropped while an older value survives resurrects deleted
// data, so a compaction that is not bottom-most keeps every tombstone.
TEST(Compaction, ACompactionThatIsNotBottomMostKeepsEveryTombstone) {
  // Again the tombstone is not at the top sequence, so what moves between the
  // two runs below is `bottom_most` and nothing else.
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, true, ""}, {"b", 10, false, "b10"}}};
  const CompactResult bottom = Compact(in, {10}, true);
  EXPECT_EQ(1u, bottom.stats.emitted);
  EXPECT_EQ(1u, bottom.stats.dropped);
  const CompactResult not_bottom = Compact(in, {10}, false);
  EXPECT_EQ(2u, not_bottom.stats.emitted);
  EXPECT_EQ(0u, not_bottom.stats.dropped);
}

// THE RESURRECTION RULE, JUDGED BY THE INSTRUMENT BUILT FOR IT. A count cannot
// see this: the tombstone and the value it masks are both single entries, and
// dropping the tombstone alone leaves an output whose SIZE is unremarkable and
// whose meaning is that a deleted key came back.
//
// The tombstone is deliberately NOT at the top sequence -- "b" at 10 carries
// that -- because the watermark pin would otherwise keep it for a reason that
// has nothing to do with the rule under test.
TEST(Compaction, ATombstoneIsKeptWhileAnythingItMasksSurvives) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, true, ""}, {"a", 4, false, "old"}, {"b", 10, false, "b10"}}};
  const std::vector<SeqNum> s = {5, 10};
  const CompactResult r = Compact(in, s);
  ASSERT_FALSE(r.output.empty());
  const rig::DropVerdict v =
      AdjudicateDrops(ModelOf(in, s), rig::ImageHoldingTables(kDir, {r.output}), kDir);
  EXPECT_TRUE(v.ok()) << v.why;
  // A reader at 10 must still see nothing for "a": the snapshot at 5 keeps the
  // value alive, so the deletion above it has work left to do.
  EXPECT_EQ(3u, r.stats.emitted);
}

TEST(Compaction, ASnapshotBelowATombstoneKeepsIt) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, true, ""}, {"a", 4, false, "old"}}};
  // A snapshot at 5 still answers "old" for "a", so neither entry may go.
  const CompactResult r = Compact(in, {5, 9});
  EXPECT_EQ(2u, r.stats.emitted);
  const rig::MergeVerdict v = AdjudicateMerge(ModelOf(in, {5, 9}), r.inputs, r.output);
  EXPECT_TRUE(v.ok()) << v.why;
}

// ------------------------------------------------------- B3-D7a's two halves

// THE TERMINATION ASSERTION, AND ITS BOUND IS EXACT. A correct compaction
// consumes each input entry exactly once, so it stops AT the bound. Reaching it
// is not a failure; exceeding it aborts inside the loop.
TEST(Compaction, EveryInputEntryIsConsumedExactlyOnce) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, false, "1"}, {"c", 8, false, "2"}},
      {{"b", 7, false, "3"}, {"d", 6, false, "4"}},
      {{"a", 5, false, "5"}, {"e", 4, false, "6"}}};
  const CompactResult r = Compact(in, {9});
  EXPECT_EQ(6u, r.bound);
  EXPECT_EQ(r.bound, r.stats.inputs_consumed);
  EXPECT_EQ(r.stats.inputs_consumed, r.stats.emitted + r.stats.dropped);
  // AND THE BOUND IS THE OTHER INSTRUMENT'S MEASUREMENT -- GF-13. It cannot be
  // raised without contradicting what the classifier counted.
  EXPECT_EQ(rig::InputEntryCount(r.inputs), r.stats.inputs_consumed);
}

// GF-12, MADE CONCRETE. The bound says the loop stops; it says nothing about
// order. A merge across three overlapping inputs is where a wrong traversal
// would show, and only the merge adjudicator can see it.
TEST(Compaction, ThreeOverlappingInputsComeOutInOneAscendingRun) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, false, "a9"}, {"m", 8, false, "m8"}},
      {{"a", 7, false, "a7"}, {"z", 6, false, "z6"}},
      {{"b", 5, false, "b5"}, {"m", 4, false, "m4"}}};
  const std::vector<SeqNum> s = {4, 5, 6, 7, 8, 9};
  const CompactResult r = Compact(in, s);
  EXPECT_EQ(6u, r.stats.emitted);
  const rig::MergeVerdict v = AdjudicateMerge(ModelOf(in, s), r.inputs, r.output);
  EXPECT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(6u, v.expected_entries);
  EXPECT_EQ(6u, v.output_entries);
}

// ------------------------------------------------------------ the watermark pin

// THE OBLIGATION THE DROP CLAIM DOES NOT STATE. The claim is about the ANSWER a
// reader gets; the watermark is a promise about a SEQUENCE. Dropping the
// highest-sequenced entry preserves every answer and destroys the engine's only
// proof of a promise it already made.
TEST(Compaction, TheHighestSequenceSurvivesEvenWhenTheClaimPermitsDroppingIt) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, true, ""}, {"a", 4, false, "old"}}};
  const CompactResult r = Compact(in, {9});
  EXPECT_EQ(1u, r.stats.pinned);
  EXPECT_EQ(1u, r.stats.emitted);
  ASSERT_FALSE(r.output.empty());
  // The output's own bytes carry the sequence, which is where Open re-derives
  // the durable floor from -- the manifest may not record it (D7).
  TestEnvironment t;
  EXPECT_TRUE(t.env()->CreateDir(kDir).ok());
  const std::string path = kDir + "/000009.sst";
  {
    WritableFilePtr f;
    EXPECT_TRUE(t.env()->NewWritableFile(path, &f).ok());
    EXPECT_TRUE(f->Append(Slice(r.output)).ok());
    EXPECT_TRUE(f->Sync().ok());
    EXPECT_TRUE(f->Close().ok());
  }
  std::shared_ptr<sst::Table> opened;
  ASSERT_TRUE(sst::Table::Open(t.env(), path, 9, &opened).ok());
  EXPECT_EQ(9u, opened->check().largest_seq);
}

// GF-14: THE OTHER HALF. Without it, "the pin fires" could be produced by a
// compaction that never drops anything, and nothing would say so.
TEST(Compaction, ThePinDoesNotFireWhenTheHighestSequenceSurvivesAnyway) {
  const std::vector<std::vector<Cell>> in = {
      {{"a", 9, false, "new"}, {"a", 4, false, "old"}}};
  const CompactResult r = Compact(in, {9});
  EXPECT_EQ(0u, r.stats.pinned);
  EXPECT_EQ(1u, r.stats.emitted);
  EXPECT_EQ(1u, r.stats.dropped);
}

}  // namespace
}  // namespace rift
