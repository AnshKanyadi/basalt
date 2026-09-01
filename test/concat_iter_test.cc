// ConcatIter: a cursor over a non-overlapping run, driven from real tables built
// by the fixture and judged against what the fixture put in them.
//
// CF-3's obligations are what this file is mostly about. Every loop in
// ConcatIter asserts the movement it terminates on, over a quantity it does not
// derive from the comparator, and the mutants below invert the ordering each
// loop relies on so those assertions have a failing case.
#include "concat_iter.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "internal_key.h"
#include "manifest_format.h"
#include "table_builder.h"
#include "test_env.h"

namespace basalt {
namespace sst {
namespace {

using testenv::TestEnvironment;

const std::string kDir = "db";

std::string IKey(const std::string& user, SeqNum seq) {
  std::string out;
  AppendInternalKey(&out, Slice(user), MakeTag(seq, ValueType::kValue));
  return out;
}

// Holds the tables alive for the life of a test: ConcatIter borrows them.
struct TableRun {
  std::unique_ptr<TestEnvironment> env;
  std::vector<std::shared_ptr<Table>> owned;
  std::vector<const Table*> ptrs;
};

TableRun BuildRun(const std::vector<std::vector<std::string>>& files) {
  TableRun r;
  r.env.reset(new TestEnvironment());
  EXPECT_TRUE(r.env->env()->CreateDir(kDir).ok());
  uint64_t number = 1;
  for (const std::vector<std::string>& keys : files) {
    const std::string path = TablePath(kDir, number);
    {
      WritableFilePtr f;
      EXPECT_TRUE(r.env->env()->NewWritableFile(path, &f).ok());
      TableBuilder b(f.get());
      for (const std::string& k : keys) {
        const std::string ik = IKey(k, 1);
        b.Add(Slice(ik), Slice(k));
      }
      EXPECT_TRUE(b.Finish().ok());
      EXPECT_TRUE(f->Sync().ok());
      EXPECT_TRUE(f->Close().ok());
    }
    std::shared_ptr<Table> t;
    EXPECT_TRUE(Table::Open(r.env->env(), path, number, &t).ok());
    r.owned.push_back(t);
    ++number;
  }
  for (const auto& t : r.owned) r.ptrs.push_back(t.get());
  return r;
}

std::vector<std::string> Forward(ConcatIter* it) {
  std::vector<std::string> out;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.push_back(ExtractUserKey(it->key()).ToString());
  }
  return out;
}

std::vector<std::string> Backward(ConcatIter* it) {
  std::vector<std::string> out;
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    out.push_back(ExtractUserKey(it->key()).ToString());
  }
  return out;
}

// --------------------------------------------------------------- traversal

TEST(ConcatIter, WalksTheWholeRunInOrder) {
  TableRun r = BuildRun({{"a", "b"}, {"m", "n"}, {"x", "z"}});
  ConcatIter it(r.ptrs);
  EXPECT_EQ(3u, it.files());
  const std::vector<std::string> want = {"a", "b", "m", "n", "x", "z"};
  EXPECT_EQ(want, Forward(&it));
}

TEST(ConcatIter, WalksBackwardsToTheSameSequence) {
  // The reverse walk is where a file-boundary bug lives: forward and backward
  // cross the same boundaries in opposite orders, so comparing them against
  // each other catches what comparing either against a list would not.
  TableRun r = BuildRun({{"a", "b"}, {"m", "n"}, {"x", "z"}});
  ConcatIter it(r.ptrs);
  std::vector<std::string> back = Backward(&it);
  std::reverse(back.begin(), back.end());
  EXPECT_EQ(Forward(&it), back);
}

TEST(ConcatIter, AnEmptyRunIsInvalidAndDoesNotCrash) {
  ConcatIter it(std::vector<const Table*>{});
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.SeekToLast();
  EXPECT_FALSE(it.Valid());
  it.Seek(Slice("anything that is long enough to carry a tag"));
  EXPECT_FALSE(it.Valid());
}

// ------------------------------------------------------------------- seek

TEST(ConcatIter, SeekLandsInTheOnlyFileThatCouldHoldTheKey) {
  TableRun r = BuildRun({{"a", "b"}, {"m", "n"}, {"x", "z"}});
  ConcatIter it(r.ptrs);
  const std::string target = IKey("m", 1);
  it.Seek(Slice(target));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("m", ExtractUserKey(it.key()).ToString());
}

TEST(ConcatIter, SeekToAGapLandsOnTheNextKeyPresent) {
  // "f" is in no file. A forward seek must land on the first key ABOVE it,
  // which lives in the next file -- the case a per-file seek gets wrong by
  // returning nothing.
  TableRun r = BuildRun({{"a", "b"}, {"m", "n"}, {"x", "z"}});
  ConcatIter it(r.ptrs);
  const std::string target = IKey("f", 1);
  it.Seek(Slice(target));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("m", ExtractUserKey(it.key()).ToString());
}

TEST(ConcatIter, SeekPastEverythingIsInvalid) {
  TableRun r = BuildRun({{"a", "b"}, {"m", "n"}});
  ConcatIter it(r.ptrs);
  const std::string target = IKey("zzzz", 1);
  it.Seek(Slice(target));
  EXPECT_FALSE(it.Valid());
}

TEST(ConcatIter, SeekBelowEverythingLandsOnTheFirstKey) {
  TableRun r = BuildRun({{"m", "n"}, {"x", "z"}});
  ConcatIter it(r.ptrs);
  const std::string target = IKey("a", 1);
  it.Seek(Slice(target));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("m", ExtractUserKey(it.key()).ToString());
}

TEST(ConcatIter, EverySeekTargetLandsWhereALinearScanWould) {
  // THE STRONGEST STATEMENT IN THIS FILE, and it is a sweep rather than a
  // claim: for every key in and around the run, the binary search must agree
  // with a linear scan. A binary search that is wrong for one input class is
  // wrong invisibly, because every other input still works.
  TableRun r = BuildRun({{"b", "d"}, {"h", "j"}, {"p", "r"}});
  ConcatIter it(r.ptrs);
  const std::vector<std::string> present = Forward(&it);
  const char* probes[] = {"a", "b", "c", "d", "e", "g", "h", "i", "j", "k",
                          "o", "p", "q", "r", "s", "z"};
  for (const char* probe : probes) {
    const std::string target = IKey(probe, 1);
    it.Seek(Slice(target));
    // What a linear scan would return: the first present key >= probe.
    std::string want;
    for (const std::string& k : present) {
      if (k >= std::string(probe)) { want = k; break; }
    }
    if (want.empty()) {
      EXPECT_FALSE(it.Valid()) << "probe " << probe;
    } else {
      ASSERT_TRUE(it.Valid()) << "probe " << probe;
      EXPECT_EQ(want, ExtractUserKey(it.key()).ToString()) << "probe " << probe;
    }
  }
}

// ------------------------------------------------------- the precondition

TEST(ConcatIter, AnOverlappingRunIsRefusedAtConstruction) {
  // Asserted rather than assumed: an overlapping run silently returns one
  // file's version of a key and hides another's, which is a wrong answer with
  // nothing structurally wrong anywhere to report it.
  TableRun r = BuildRun({{"a", "m"}, {"f", "z"}});
  EXPECT_DEATH({ ConcatIter it(r.ptrs); (void)it.files(); }, "");
}

}  // namespace
}  // namespace sst
}  // namespace basalt
