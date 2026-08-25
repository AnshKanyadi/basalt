// The SSTable writer, checked against rules that were already seen to reject
// every illegal shape. Not against a decoder written to agree with it.
//
// Every table this file produces goes through ValidateTable -- the same
// function, unchanged, that sst_format_test.cc induced on hand-built bytes.
// That is the whole value of B2-D6's ordering: the writer's output is judged by
// a classifier that has never seen it.
#include "table_builder.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bloom.h"
#include "env.h"
#include "internal_key.h"
#include "memtable.h"
#include "sha256.h"
#include "table_check.h"
#include "test_env.h"

namespace rift {
namespace sst {
namespace {

using testenv::FaultPlan;
using testenv::Injection;
using testenv::TestEnvironment;

const std::string kDir = "db";
const std::string kTable = "db/000007.sst";

std::string IKey(const std::string& user, SeqNum seq,
                 ValueType type = ValueType::kValue) {
  std::string out;
  AppendInternalKey(&out, Slice(user), MakeTag(seq, type));
  return out;
}

// Writes one table through the Env and returns its bytes. The caller supplies
// the entries in table order; the writer RIFT_CHECKs that they are.
struct Written {
  std::string bytes;
  uint64_t entries = 0;
  std::string smallest;
  std::string largest;
  SeqNum largest_seq = 0;
  Status status;
};

Written WriteTable(TestEnvironment* t,
                   const std::vector<std::pair<std::string, std::string>>& kvs) {
  Written w;
  WritableFilePtr f;
  EXPECT_TRUE(t->env()->CreateDir(kDir).ok());
  EXPECT_TRUE(t->env()->NewWritableFile(kTable, &f).ok());
  {
    TableBuilder b(f.get());
    for (const auto& kv : kvs) b.Add(Slice(kv.first), Slice(kv.second));
    w.status = b.Finish();
    w.entries = b.entries();
    w.smallest = b.smallest().ToString();
    w.largest = b.largest().ToString();
    w.largest_seq = b.largest_seq();
  }
  if (w.status.ok()) {
    EXPECT_TRUE(f->Sync().ok());
    EXPECT_TRUE(f->Close().ok());
  }
  w.bytes = t->ContentNow(kTable);
  return w;
}

std::vector<std::pair<std::string, std::string>> Sequential(int n, int value_bytes) {
  std::vector<std::pair<std::string, std::string>> out;
  for (int i = 0; i < n; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "key%06d", i);
    out.emplace_back(IKey(buf, 1), std::string(static_cast<std::size_t>(value_bytes), 'v'));
  }
  return out;
}

// ----------------------------------------------------------- the writer

TEST(SstWriter, WhatItWritesTheClassifierAccepts) {
  TestEnvironment t;
  const Written w = WriteTable(&t, {{IKey("a", 1), "1"},
                                    {IKey("b", 1), "2"},
                                    {IKey("c", 1), "3"}});
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(1u, v.data_blocks);
  EXPECT_EQ(3u, v.entries);
  EXPECT_EQ(3u, w.entries);
}

TEST(SstWriter, ManyBlocksAreStillOneLegalTable) {
  // Enough data to cut several blocks, so the index has more than one entry and
  // the separator rule is exercised on a table the writer actually cut.
  TestEnvironment t;
  const Written w = WriteTable(&t, Sequential(400, 64));
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_GT(v.data_blocks, 5u) << "the block size never cut a block";
  EXPECT_EQ(400u, v.entries);
}

TEST(SstWriter, VersionsOfOneKeyAreWrittenNewestFirst) {
  // The order the memtable emits, written out and validated. A writer that
  // reordered these would be caught by the classifier, and a classifier
  // comparing bytes would refuse this legal table -- the pair is what makes
  // either statement mean anything.
  TestEnvironment t;
  const Written w = WriteTable(&t, {{IKey("a", 9), "new"},
                                    {IKey("a", 4), "old"},
                                    {IKey("b", 2, ValueType::kDeletion), ""}});
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(3u, v.entries);
  EXPECT_EQ(IKey("a", 9), w.smallest);
  EXPECT_EQ(IKey("b", 2, ValueType::kDeletion), w.largest);
  // The largest SEQUENCE is not the sequence of the largest KEY. It is what
  // recovery re-derives and holds the manifest to (B2-D4 section 5.1 point 2),
  // so a writer that reported the last key's sequence would put a number in the
  // manifest that recovery then disagrees with.
  EXPECT_EQ(9u, w.largest_seq);
}

TEST(SstWriter, TheFilterHoldsEveryUserKeyAndNotTheInternalOnes) {
  // A filter built over INTERNAL keys is the defect that makes every lookup
  // miss: a reader probes a user key, the filter holds user_key||tag, and the
  // answer is "definitely not" for keys the table certainly contains.
  TestEnvironment t;
  std::vector<std::pair<std::string, std::string>> kvs;
  std::vector<std::string> users;
  for (int i = 0; i < 200; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "user%04d", i);
    users.emplace_back(buf);
    kvs.emplace_back(IKey(buf, 3), "v");
  }
  const Written w = WriteTable(&t, kvs);
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();

  Footer footer;
  std::string why;
  ASSERT_TRUE(DecodeFooter(Slice(w.bytes), &footer, &why)) << why;
  ASSERT_GT(footer.filter.size, 0u);
  const std::string filter_block =
      w.bytes.substr(footer.filter.offset, footer.filter.size);
  FilterReader r;
  ASSERT_TRUE(FilterReader::Parse(Slice(filter_block), &r, &why)) << why;
  for (const std::string& u : users) {
    EXPECT_TRUE(r.MayContain(Slice(u))) << "the filter denies a key the table holds: " << u;
  }
}

TEST(SstWriter, TheBytesArePinned) {
  // Same workload, same bytes, forever. Catches ambient randomness, an
  // uninitialized byte reaching the file, and any float that reached the sizing
  // arithmetic -- the WAL digest's three-for-one, on the second format.
  TestEnvironment t;
  const Written w = WriteTable(&t, Sequential(64, 16));
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  EXPECT_EQ("5ac666d3fcbf1b1c92e2423cf56dd6081b284b682c07d5df96a2bf9099be5fae",
            wal::Sha256Hex(w.bytes.data(), w.bytes.size()));
}

TEST(SstWriter, AMemtableWrittenOutIsALegalTable) {
  // B2.4's PRECONDITION, END TO END, before the flush path exists: whatever the
  // memtable emits, in the order it emits it, is a table the classifier accepts.
  MemTable m;
  for (SeqNum seq = 1; seq <= 4; ++seq) {
    for (int i = 0; i < 50; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "k%04d", i);
      const std::string user(buf);
      const std::string value = user + ":" + std::to_string(seq);
      m.Add(seq, seq == 3 ? ValueType::kDeletion : ValueType::kValue,
            Slice(user), Slice(value));
    }
  }
  std::vector<std::pair<std::string, std::string>> kvs;
  MemTable::Iter it(&m);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    std::string ikey;
    AppendInternalKey(&ikey, it.user_key(), it.tag());
    kvs.emplace_back(ikey, it.value().ToString());
  }
  ASSERT_EQ(200u, kvs.size());

  TestEnvironment t;
  const Written w = WriteTable(&t, kvs);
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  const TableCheck v = ValidateTable(Slice(w.bytes));
  ASSERT_TRUE(v.ok()) << TableFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(200u, v.entries);
}

// -------------------------------------------------------- and when I/O fails

TEST(SstWriter, AFailedAppendIsReportedAndNotSwallowed) {
  // A writer that returned ok after a failed write would produce a file the
  // flush path then names in the manifest -- which is B2-D5's ordering giving
  // a guarantee about a table that was never written.
  //
  // The ordinal is found by RUNNING the workload once and reading the harness's
  // own ledger, rather than by counting Env calls in one's head: a hand-counted
  // ordinal is a fixture that silently stops pointing at the call it named.
  TestEnvironment probe;
  const Written clean = WriteTable(&probe, Sequential(400, 64));
  ASSERT_TRUE(clean.status.ok());
  uint64_t third_append = 0;
  int seen = 0;
  for (const auto& e : probe.ledger()) {
    if (e.site == CallSite::kWritableFileAppend && ++seen == 3) {
      third_append = e.ordinal;
      break;
    }
  }
  ASSERT_GT(third_append, 0u) << "the workload never appended three times";

  FaultPlan plan;
  plan.At(third_append, Injection::kIoError);
  TestEnvironment t(plan);
  const Written w = WriteTable(&t, Sequential(400, 64));
  EXPECT_FALSE(w.status.ok());
  // And the writer stopped: nothing after the failed append reached the file.
  EXPECT_LT(w.bytes.size(), clean.bytes.size());
}

}  // namespace
}  // namespace sst
}  // namespace rift
