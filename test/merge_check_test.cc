// The merge adjudicator, driven from fixture tables, WITH NO MERGE IN THE TREE.
//
// Fifth use of the ordering, and here the reason is sharper than usual: the
// merge is the thing whose output ORDER is in question, so a checker written
// afterwards would take its notion of the right order from the code that
// produced it.
//
// EVERY REFUSAL BELOW IS A CASE THE DROP ADJUDICATOR PASSES. That is the point:
// GF-12 one level up -- an instrument correct about what it checks and silent
// about what a reader assumes it covers.
#include "merge_check.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "drop_check.h"
#include "internal_key.h"
#include "manifest.h"
#include "table_builder.h"
#include "test_env.h"

namespace rift {
namespace rig {
namespace {

using testenv::TestEnvironment;

const std::string kDir = "db";

struct Cell { std::string key; ModelSeq seq; std::string value; };

std::string IKey(const std::string& k, ModelSeq seq) {
  std::string out;
  AppendInternalKey(&out, Slice(k), MakeTag(seq, ValueType::kValue));
  return out;
}

// Builds one SSTable image from cells given in table order.
std::string Table(const std::vector<Cell>& cells) {
  auto t = std::unique_ptr<TestEnvironment>(new TestEnvironment());
  EXPECT_TRUE(t->env()->CreateDir(kDir).ok());
  const std::string path = kDir + "/000001.sst";
  {
    WritableFilePtr f;
    EXPECT_TRUE(t->env()->NewWritableFile(path, &f).ok());
    sst::TableBuilder b(f.get());
    for (const Cell& c : cells) {
      const std::string k = IKey(c.key, c.seq);
      b.Add(Slice(k), Slice(c.value));
    }
    EXPECT_TRUE(b.Finish().ok());
    EXPECT_TRUE(f->Sync().ok());
    EXPECT_TRUE(f->Close().ok());
  }
  return t->ContentNow(path);
}


// A durable image holding one table, WITH A MANIFEST NAMING IT -- because a
// table the manifest does not name is an orphan, and an adjudicator that counted
// orphans would let a compaction "keep" a record in a file nothing refers to.
ImageBytes ImageHolding(const std::string& table_bytes) {
  auto t = std::unique_ptr<TestEnvironment>(new TestEnvironment());
  EXPECT_TRUE(t->env()->CreateDir(kDir).ok());
  sst::ManifestState state;
  std::unique_ptr<sst::Manifest> m;
  std::vector<std::shared_ptr<sst::Table>> tables;
  EXPECT_TRUE(sst::Manifest::Open(t->env(), kDir, &state, &tables, &m).ok());
  const uint64_t number = state.next_file_number;
  const std::string path = sst::TablePath(kDir, number);
  {
    WritableFilePtr f;
    EXPECT_TRUE(t->env()->NewWritableFile(path, &f).ok());
    EXPECT_TRUE(f->Append(Slice(table_bytes)).ok());
    EXPECT_TRUE(f->Sync().ok());
    EXPECT_TRUE(f->Close().ok());
    DirectoryPtr d;
    EXPECT_TRUE(t->env()->NewDirectory(kDir, &d).ok());
    EXPECT_TRUE(d->Sync().ok());
    EXPECT_TRUE(d->Close().ok());
  }
  std::shared_ptr<sst::Table> opened;
  EXPECT_TRUE(sst::Table::Open(t->env(), path, number, &opened).ok());
  sst::TableMeta meta;
  meta.number = number;
  meta.file_bytes = opened->file_bytes();
  meta.smallest = opened->check().smallest_key;
  meta.largest = opened->check().largest_key;
  meta.largest_seq = opened->check().largest_seq;
  sst::ManifestEdit add;
  add.kind = sst::EditKind::kAddTable;
  add.table = meta;
  sst::ManifestEdit bump;
  bump.kind = sst::EditKind::kNextFileNumber;
  bump.number = number + 1;
  EXPECT_TRUE(m->AppendGroup({add, bump}).ok());
  EXPECT_TRUE(m->Close().ok());
  return t->Image();
}

VersionModel ModelOf(const std::vector<Cell>& cells) {
  VersionModel m;
  for (const Cell& c : cells) m.NoteWrite(c.key, c.seq, false, c.value);
  return m;
}

// ------------------------------------------------------------ the bound

TEST(MergeCheck, TheBoundIsCountedFromTheInputsAndNotChosen) {
  // GF-13: the merge's progress bound is the sum of the inputs' entry counts,
  // measured by the same decoder the classifier uses. There is no number in the
  // source to raise -- raising it would require claiming a table holds more
  // entries than it holds.
  const std::string a = Table({{"a", 1, "1"}, {"b", 1, "2"}});
  const std::string b = Table({{"c", 1, "3"}});
  EXPECT_EQ(3u, InputEntryCount({a, b}));
  EXPECT_EQ(0u, InputEntryCount({}));
}

// ------------------------------------------------- the legal merge

TEST(MergeCheck, AcceptsAnOutputThatIsExactlyTheMergeOfItsInputs) {
  const std::vector<Cell> all = {{"a", 1, "1"}, {"b", 1, "2"}, {"c", 1, "3"}};
  const VersionModel model = ModelOf(all);
  const std::string in1 = Table({{"a", 1, "1"}, {"c", 1, "3"}});
  const std::string in2 = Table({{"b", 1, "2"}});
  const std::string out = Table(all);

  const MergeVerdict v = AdjudicateMerge(model, {in1, in2}, out);
  ASSERT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(3u, v.input_entries);
  EXPECT_EQ(3u, v.output_entries);
  EXPECT_EQ(3u, v.expected_entries);
}

// ------------------- the three failures the drop adjudicator cannot see

TEST(MergeCheck, RefusesAnOutputWhoseValuesAreShiftedByOne) {
  // THE EXAMPLE FROM THE RULING, MADE A TEST. Every required (user_key, seq) is
  // present, so the drop adjudicator passes -- and every value belongs to the
  // wrong key.
  const std::vector<Cell> all = {{"a", 1, "1"}, {"b", 1, "2"}, {"c", 1, "3"}};
  const VersionModel model = ModelOf(all);
  const std::string in = Table(all);
  const std::string shifted = Table({{"a", 1, "2"}, {"b", 1, "3"}, {"c", 1, "1"}});

  const MergeVerdict v = AdjudicateMerge(model, {in}, shifted);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(std::string::npos, v.why.find("changed a value")) << v.why;

  // AND THE DROP ADJUDICATOR PASSES THE SAME STATE, which is why this
  // instrument exists. Asserted rather than asserted-about, so the claim that
  // the two are complementary has a failing case if it stops being true.
  //
  // THE IMAGE NEEDS A MANIFEST NAMING THE TABLE. The first version of this
  // fixture handed the adjudicator a bare .sst, which it correctly read as an
  // ORPHAN -- a table nothing refers to -- and reported every version as
  // dropped. It was right about the image it was given; the image did not
  // describe what its author meant.
  const ImageBytes image = ImageHolding(shifted);
  const DropVerdict d = AdjudicateDrops(model, image, kDir);
  EXPECT_TRUE(d.ok()) << "the drop adjudicator saw the shift; the claim that it "
                         "is blind to values needs revisiting: " << d.why;
}

TEST(MergeCheck, RefusesAnOutputWhoseEntriesAreOutOfOrder) {
  // Built by hand, because TableBuilder RIFT_CHECKs ascending order -- which is
  // the writer refusing to produce the bytes a wrong MERGE would. So the
  // fixture assembles the block directly.
  const std::vector<Cell> all = {{"a", 1, "1"}, {"b", 1, "2"}};
  const VersionModel model = ModelOf(all);
  const std::string in = Table(all);

  // The classifier would refuse this table too; the point is that the merge
  // adjudicator refuses it for the RIGHT reason and says so.
  auto t = std::unique_ptr<TestEnvironment>(new TestEnvironment());
  EXPECT_TRUE(t->env()->CreateDir(kDir).ok());
  sst::BlockBuilder data;
  const std::string kb = IKey("b", 1), ka = IKey("a", 1);
  data.Add(Slice(kb), Slice("2"));
  data.Add(Slice(ka), Slice("1"));   // descending: no writer emits this
  const std::string block = data.Finish();
  std::string image = block;
  sst::BlockHandle dh;
  dh.offset = 0;
  dh.size = static_cast<uint32_t>(block.size());
  sst::BlockBuilder index;
  std::string handle;
  sst::EncodeHandle(dh, &handle);
  index.Add(Slice(ka), Slice(handle));
  const std::string ib = index.Finish();
  sst::BlockHandle ih;
  ih.offset = image.size();
  ih.size = static_cast<uint32_t>(ib.size());
  image += ib;
  sst::Footer footer;
  footer.index = ih;
  footer.format_version = sst::kFormatVersion;
  sst::EncodeFooter(footer, &image);

  const MergeVerdict v = AdjudicateMerge(model, {in}, image);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(std::string::npos, v.why.find("out of order")) << v.why;
}

TEST(MergeCheck, RefusesAnOutputThatLostARequiredEntry) {
  const std::vector<Cell> all = {{"a", 1, "1"}, {"b", 1, "2"}};
  const VersionModel model = ModelOf(all);
  const std::string in = Table(all);
  const std::string out = Table({{"a", 1, "1"}});
  const MergeVerdict v = AdjudicateMerge(model, {in}, out);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(std::string::npos, v.why.find("lost a required entry")) << v.why;
}

TEST(MergeCheck, RefusesAnOutputThatInventedAnEntry) {
  const std::vector<Cell> all = {{"a", 1, "1"}};
  const VersionModel model = ModelOf(all);
  const std::string in = Table(all);
  const std::string out = Table({{"a", 1, "1"}, {"z", 9, "invented"}});
  const MergeVerdict v = AdjudicateMerge(model, {in}, out);
  ASSERT_FALSE(v.ok());
  EXPECT_NE(std::string::npos, v.why.find("no input contained")) << v.why;
}

TEST(MergeCheck, APermittedDropIsNotAViolation) {
  // The other direction: a superseded version nobody can reach may be dropped,
  // and an adjudicator that refused that would refuse compaction itself.
  const std::vector<Cell> all = {{"a", 1, "old"}, {"a", 5, "new"}};
  const VersionModel model = ModelOf(all);
  const std::string in = Table({{"a", 5, "new"}, {"a", 1, "old"}});
  const std::string out = Table({{"a", 5, "new"}});
  const MergeVerdict v = AdjudicateMerge(model, {in}, out);
  ASSERT_TRUE(v.ok()) << v.why;
  EXPECT_EQ(2u, v.input_entries);
  EXPECT_EQ(1u, v.output_entries);
  EXPECT_EQ(1u, v.expected_entries);
}

}  // namespace
}  // namespace rig
}  // namespace rift
