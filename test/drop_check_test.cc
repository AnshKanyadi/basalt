// The drop adjudicator, driven from fixture images, with NO COMPACTION IN THE
// TREE.
//
// This is the observer-before-the-observed rule at its most important point in
// the phase. B3-D2: the adjudicator is the only thing that can distinguish a
// policy that drops correctly from one that drops too much, and **an
// adjudicator written after the policy will agree with it.** So it is written
// first, and every refusal below is induced against a durable image that no
// compaction produced.
//
// THE PAIR AT THE CENTRE OF THIS FILE is `ASnapshotMakesAnOlderVersionRequired`
// and `WithoutASnapshotTheSameDropIsPermitted`. The images are IDENTICAL. The
// state read at the current sequence is IDENTICAL. Only the snapshot set
// differs, and one is a violation while the other is correct -- which is the
// whole argument for why B2's state comparisons cannot do this job.
#include "drop_check.h"

#include "image_fixture.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "manifest.h"
#include "wal.h"
#include "table_builder.h"
#include "test_env.h"
#include "version_model.h"

namespace basalt {
namespace rig {
namespace {

using testenv::TestEnvironment;

const std::string kDir = "db";

struct Entry {
  std::string user_key;
  ModelSeq seq;
  bool deletion;
  std::string value;
};

// Builds a durable image holding exactly `in_table`.
//
// ONE CONSTRUCTION PATH, in rig/image_fixture.h. The first version of this
// fixture wrote the table, synced it, and never synced the DIRECTORY -- so the
// name was not durable, the image did not contain the table, and the
// adjudicator correctly reported every version as dropped. It was right about
// the image it was given. Building through the engine's own install sequence is
// what makes that class unreachable rather than remembered.
ImageBytes BuildImage(const std::vector<Entry>& in_table) {
  if (in_table.empty()) return ImageHoldingTables(kDir, {});
  std::vector<FixtureCell> cells;
  for (const Entry& e : in_table) {
    FixtureCell c;
    c.user_key = e.user_key;
    c.seq = e.seq;
    c.deletion = e.deletion;
    c.value = e.value;
    cells.push_back(c);
  }
  return ::basalt::rig::BuildImage(kDir, {cells});
}

// The WAL the fixture's Manifest::Open created holds nothing, so every version
// the model knows about must come from the table -- which is what makes a
// missing one a DROP rather than a record still in flight.
VersionModel ModelOf(const std::vector<Entry>& submitted) {
  VersionModel model;
  for (const Entry& e : submitted) {
    model.NoteWrite(e.user_key, e.seq, e.deletion, e.value);
  }
  return model;
}

// ------------------------------------------------------- the model itself

TEST(VersionModel, RequiredIsAtMostOnePerObservableSequence) {
  VersionModel m;
  m.NoteWrite("k", 1, false, "a");
  m.NoteWrite("k", 5, false, "b");
  m.NoteWrite("k", 9, false, "c");

  // No snapshots: only the current view is observable, so only the newest
  // version of each key is required.
  EXPECT_EQ(1u, m.ObservableSequences().size());
  EXPECT_EQ(1u, m.Required().size());
  EXPECT_EQ(3u, m.All().size());

  m.NoteSnapshotTaken(3);
  m.NoteSnapshotTaken(7);
  EXPECT_EQ(3u, m.ObservableSequences().size());
  // seq 3 resolves to version 1; seq 7 to version 5; current to version 9.
  const std::set<VersionId> req = m.Required();
  EXPECT_EQ(3u, req.size());
  EXPECT_EQ(1u, req.count({"k", 1}));
  EXPECT_EQ(1u, req.count({"k", 5}));
  EXPECT_EQ(1u, req.count({"k", 9}));

  // RELEASING A SNAPSHOT MAKES MORE DROPPABLE, and that direction is the only
  // one that can: a snapshot is taken at the CURRENT sequence, so it can never
  // reach a version an existing floor already permitted dropping. B3-D1 section
  // 1.3 -- the floor only moves up.
  m.NoteSnapshotReleased(3);
  EXPECT_EQ(2u, m.Required().size());
}

TEST(VersionModel, ASnapshotBelowEveryVersionRequiresNothingOfThatKey) {
  VersionModel m;
  m.NoteWrite("k", 5, false, "a");
  m.NoteSnapshotTaken(2);  // nothing of k is visible at 2
  const std::set<VersionId> req = m.Required();
  EXPECT_EQ(1u, req.size());
  EXPECT_EQ(1u, req.count({"k", 5}));
}

// ------------------------------------------- the pair the whole phase rests on

TEST(DropCheck, ASnapshotMakesAnOlderVersionRequired) {
  // THE FAILURE NO STATE COMPARISON CAN SEE. A read at the current sequence
  // returns "b" whether or not version 1 survives. Only a read through the
  // snapshot at 3 differs, and B2's checks never take one.
  const std::vector<Entry> submitted = {{"k", 1, false, "a"}, {"k", 5, false, "b"}};
  VersionModel model = ModelOf(submitted);
  model.NoteSnapshotTaken(3);

  const ImageBytes image = BuildImage({{"k", 5, false, "b"}});  // version 1 dropped
  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  ASSERT_FALSE(v.ok()) << "a version a live snapshot resolves to was dropped and nothing objected";
  EXPECT_NE(std::string::npos, v.why.find("sequence 1")) << v.why;
  EXPECT_NE(std::string::npos, v.why.find("snapshot")) << v.why;
  EXPECT_EQ(2u, v.required_total);
}

TEST(DropCheck, WithoutASnapshotTheSameDropIsPermitted) {
  // THE SAME IMAGE, THE SAME SUBMISSIONS, NO SNAPSHOT -- and it is correct.
  // Superseding a version nobody can reach is what compaction is FOR, and an
  // adjudicator that refused this would refuse compaction itself.
  const std::vector<Entry> submitted = {{"k", 1, false, "a"}, {"k", 5, false, "b"}};
  const VersionModel model = ModelOf(submitted);

  const ImageBytes image = BuildImage({{"k", 5, false, "b"}});
  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  ASSERT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(1u, v.required_total);
  EXPECT_EQ(1u, v.dropped) << "the superseded version was not counted as dropped";
  EXPECT_EQ(1u, v.tables_read);
}

// -------------------------------------------------------- the tombstone rule

TEST(DropCheck, ADeletionDroppedWhileAnOlderVersionSurvivesIsRefused) {
  // Deleted data returning is the loudest failure compaction can produce, and
  // it is the half of B3-D1's claim that constrains INPUT SELECTION rather than
  // the drop rule alone.
  const std::vector<Entry> submitted = {{"k", 1, false, "a"}, {"k", 5, true, ""}};
  const VersionModel model = ModelOf(submitted);

  // The image keeps the old value and loses the deletion.
  const ImageBytes image = BuildImage({{"k", 1, false, "a"}});
  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(std::string::npos, v.why.find("Deleted data has returned")) << v.why;
}

TEST(DropCheck, ADeletionDroppedWithNothingOlderSurvivingIsPermitted) {
  // The other direction, and it is what makes the rule a discriminator rather
  // than a prohibition: once nothing older survives, the tombstone has no work
  // left to do and keeping it forever is the actual bug.
  const std::vector<Entry> submitted = {{"k", 1, false, "a"}, {"k", 5, true, ""},
                                        {"z", 7, false, "z"}};
  VersionModel model = ModelOf(submitted);
  const ImageBytes image = BuildImage({{"z", 7, false, "z"}});
  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  ASSERT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(2u, v.dropped) << "the value and its tombstone both dropped";
}

TEST(DropCheck, AVersionStillOnlyInTheWalCountsAsSurvived) {
  // A version written and not yet flushed lives ONLY in the WAL. Counting the
  // tables alone would report every unflushed write as dropped -- which is the
  // adjudicator refusing normal operation, the inversion section 5.4 rejected
  // candidate (a) for.
  auto t = std::unique_ptr<TestEnvironment>(new TestEnvironment());
  ASSERT_TRUE(t->env()->CreateDir(kDir).ok());
  {
    sst::ManifestState state;
    std::unique_ptr<sst::Manifest> m;
    std::vector<std::shared_ptr<sst::Table>> tables;
    ASSERT_TRUE(sst::Manifest::Open(t->env(), kDir, &state, &tables, &m).ok());
    sst::ManifestEdit add_wal;
    add_wal.kind = sst::EditKind::kAddWal;
    add_wal.number = 7;
    ASSERT_TRUE(m->AppendGroup({add_wal}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  {
    std::unique_ptr<wal::Wal> w;
    ASSERT_TRUE(wal::Wal::Open(t->env(), kDir, 7, wal::Caps(), &w).ok());
    std::vector<wal::Op> ops;
    wal::Op op;
    const std::string k = "k";
    const std::string val = "v";
    op.kind = wal::OpKind::kSet;
    op.key = Slice(k);
    op.value = Slice(val);
    ops.push_back(op);
    ASSERT_TRUE(w->Apply(1, ops).ok());
    wal::SeqNum mark = 0;
    ASSERT_TRUE(w->Sync(&mark).ok());
    ASSERT_TRUE(w->Close().ok());
  }
  VersionModel model;
  model.NoteWrite("k", 1, false, "v");
  const DropVerdict v = AdjudicateDrops(model, t->Image(), kDir);
  ASSERT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(1u, v.wals_read);
  EXPECT_EQ(1u, v.survived);
  EXPECT_EQ(0u, v.dropped);
}

TEST(DropCheck, AVersionNobodyWroteIsRefused) {
  // DIRECTION THREE, AND IT IS THE ONE THAT GUARDS THE OTHER TWO. Directions one
  // and two both ask whether something is MISSING, so neither can see a reader
  // that reports a record the bytes do not contain -- and that reader makes a
  // real drop look survived, which is a FALSE PASS.
  //
  // Induced here by a model that is missing a version the image legitimately
  // holds, which is the same observation from the other side: the checker and
  // the harness disagree about what was ever written, and the checker says so
  // rather than believing the bytes.
  // Table order is user key ascending, tag DESCENDING, so the newer version
  // comes first. The builder BASALT_CHECKs it, which is how the first draft of
  // this fixture was caught.
  const ImageBytes image = BuildImage({{"k", 5, false, "b"}, {"k", 1, false, "a"}});
  VersionModel model;
  model.NoteWrite("k", 5, false, "b");  // version 1 never submitted

  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(std::string::npos, v.why.find("nobody ever wrote")) << v.why;
  EXPECT_NE(std::string::npos, v.why.find("look survived")) << v.why;
  EXPECT_EQ(1u, v.phantom);
}

TEST(DropCheck, EveryVersionSubmittedAndDurableIsNoPhantom) {
  // The other direction of the same check: an image holding exactly what was
  // submitted has no phantoms, so the guard is not simply always firing.
  const std::vector<Entry> submitted = {{"k", 1, false, "a"}, {"k", 5, false, "b"}};
  VersionModel model = ModelOf(submitted);
  model.NoteSnapshotTaken(3);
  const ImageBytes image = BuildImage({{"k", 5, false, "b"}, {"k", 1, false, "a"}});
  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  ASSERT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(0u, v.phantom);
  EXPECT_EQ(2u, v.survived);
}

// ------------------------------------------------------------ what it counts

TEST(DropCheck, AnImageHoldingEverythingRequiredPasses) {
  const std::vector<Entry> submitted = {{"a", 1, false, "1"}, {"b", 2, false, "2"},
                                        {"c", 3, false, "3"}};
  const VersionModel model = ModelOf(submitted);
  const ImageBytes image = BuildImage({{"a", 1, false, "1"}, {"b", 2, false, "2"},
                                       {"c", 3, false, "3"}});
  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  ASSERT_TRUE(v.ok()) << v.why;
  // ANY COUNT A VERDICT CARRIES IS ASSERTED OR DELETED.
  EXPECT_EQ(3u, v.survived);
  EXPECT_EQ(3u, v.required_total);
  EXPECT_EQ(0u, v.dropped);
  EXPECT_EQ(1u, v.tables_read);
  EXPECT_EQ(0u, v.wals_read) << "this fixture has no WAL; the WAL path is asserted by "
                                "AVersionStillOnlyInTheWalCountsAsSurvived";
}

TEST(DropCheck, AnOrphanTableIsNotCountedAsASurvivor) {
  // A table the manifest does not name is one a crash caught before its edit.
  // Counting it would let a compaction "keep" a record in a file nothing refers
  // to -- durable bytes that no reader can reach.
  const std::vector<Entry> submitted = {{"k", 1, false, "a"}};
  const VersionModel model = ModelOf(submitted);
  ImageBytes image = BuildImage({{"k", 1, false, "a"}});

  // Rename the live table's number out of the manifest's reach by moving its
  // bytes to a number nothing names.
  std::string live_path, bytes;
  for (const auto& e : image) {
    if (e.first.size() > 4 && e.first.compare(e.first.size() - 4, 4, ".sst") == 0) {
      live_path = e.first;
      bytes = e.second;
    }
  }
  ASSERT_FALSE(live_path.empty());
  image.erase(live_path);
  image[kDir + "/000099.sst"] = bytes;

  const DropVerdict v = AdjudicateDrops(model, image, kDir);
  EXPECT_FALSE(v.ok()) << "an orphan table was counted as holding a live version";
  EXPECT_EQ(0u, v.tables_read);
}

}  // namespace
}  // namespace rig
}  // namespace basalt
