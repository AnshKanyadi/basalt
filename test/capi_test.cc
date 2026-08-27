// THE C BOUNDARY, tested through the C API rather than around it.
//
// It links `rift_capi` and calls only `extern "C"` functions, so what is
// asserted is what a cgo caller will actually get -- not what the C++ objects
// underneath happen to do.
#include "rift.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

// A directory per test, in the real filesystem. The C boundary opens a POSIX
// Env by construction -- there is no way to pass a TestEnv across it, and there
// should not be: a caller cannot inject faults, and the boundary's job is to be
// the same engine seen from outside.
std::string TempDir(const char* name) {
  return std::string("/tmp/rift-capi-") + name;
}

void Wipe(const std::string& dir) {
  const std::string cmd = "rm -rf '" + dir + "'";
  (void)std::system(cmd.c_str());
}

struct Db {
  explicit Db(const char* name, uint64_t flush = 0) : dir(TempDir(name)) {
    Wipe(dir);
    EXPECT_EQ(RIFT_OK, rift_db_open(dir.data(), dir.size(), flush, 0, 0, &db));
  }
  ~Db() {
    if (db != nullptr) (void)rift_db_close(db);
    Wipe(dir);
  }
  std::string dir;
  rift_db* db = nullptr;
};

std::string Get(rift_db* db, const std::string& k, rift_status* st = nullptr) {
  char buf[4096];
  size_t needed = 0;
  const rift_status s = rift_db_get(db, k.data(), k.size(), buf, sizeof buf, &needed);
  if (st != nullptr) *st = s;
  if (s != RIFT_OK) return "<absent>";
  return std::string(buf, needed);
}

// ---------------------------------------------------------------- the basics

TEST(CApi, WritesAndReadsThroughTheBoundary) {
  Db d("basic");
  rift_batch* b = rift_batch_new();
  ASSERT_NE(nullptr, b);
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "a", 1, "1", 1));
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "b", 1, "2", 1));
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, b, &seq));
  rift_batch_free(b);
  EXPECT_EQ(1u, seq) << "one batch is one sequence";
  EXPECT_EQ("1", Get(d.db, "a"));
  EXPECT_EQ("2", Get(d.db, "b"));
}

TEST(CApi, NotFoundIsACodeAndNotAnEmptyValue) {
  Db d("notfound");
  rift_status st = RIFT_OK;
  EXPECT_EQ("<absent>", Get(d.db, "missing", &st));
  EXPECT_EQ(RIFT_NOT_FOUND, st) << "an absent key must be distinguishable from "
                                   "one holding the empty value";
  rift_batch* b = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "empty", 5, "", 0));
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, b, &seq));
  rift_batch_free(b);
  EXPECT_EQ("", Get(d.db, "empty", &st));
  EXPECT_EQ(RIFT_OK, st);
}

// THE CALLER SUPPLIES THE BUFFER, and a short one is TOLD THE LENGTH rather
// than truncated. Truncation would be a wrong answer that looks like a right
// one, which is the failure the length exists to prevent.
TEST(CApi, AShortBufferIsToldTheLengthAndNotTruncated) {
  Db d("short");
  const std::string big(500, 'v');
  rift_batch* b = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "k", 1, big.data(), big.size()));
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, b, &seq));
  rift_batch_free(b);

  char small[10];
  size_t needed = 0;
  EXPECT_EQ(RIFT_BUFFER_TOO_SMALL,
            rift_db_get(d.db, "k", 1, small, sizeof small, &needed));
  EXPECT_EQ(big.size(), needed);

  std::vector<char> right(needed);
  size_t again = 0;
  EXPECT_EQ(RIFT_OK, rift_db_get(d.db, "k", 1, right.data(), right.size(), &again));
  EXPECT_EQ(big, std::string(right.data(), again));
}

// NULL IS UNBOUNDED; AN EMPTY NON-NULL POINTER IS THE EMPTY KEY. db.h's
// divergence 3 surviving into C, where the distinction is the pointer.
TEST(CApi, NullBoundsAreUnboundedAndEmptyOnesAreTheEmptyKey) {
  Db d("bounds");
  rift_batch* seed = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_set(seed, "a", 1, "1", 1));
  ASSERT_EQ(RIFT_OK, rift_batch_set(seed, "b", 1, "2", 1));
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, seed, &seq));
  rift_batch_free(seed);

  // [ "", "" ) -- bounded and empty. Deletes NOTHING.
  rift_batch* empty = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_delete_range(empty, "", 0, "", 0));
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, empty, &seq));
  rift_batch_free(empty);
  EXPECT_EQ("1", Get(d.db, "a")) << "an empty bounded range deleted something";

  // [ unbounded, unbounded ) -- section 8.2's clear-everything.
  rift_batch* all = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_delete_range(all, nullptr, 0, nullptr, 0));
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, all, &seq));
  rift_batch_free(all);
  EXPECT_EQ("<absent>", Get(d.db, "a"));
  EXPECT_EQ("<absent>", Get(d.db, "b"));
}

// ---------------------------------------------------------------- iterators

std::vector<std::pair<std::string, std::string>> Drain(rift_iter* it, size_t block,
                                                       size_t key_cap = 4096,
                                                       size_t val_cap = 4096) {
  std::vector<std::pair<std::string, std::string>> out;
  std::vector<uint32_t> klen(block), vlen(block);
  std::vector<char> keys(key_cap), vals(val_cap);
  for (;;) {
    size_t filled = 0, ku = 0, vu = 0;
    const rift_status s = rift_iter_next_block(it, block, klen.data(), vlen.data(),
                                               keys.data(), keys.size(), &ku,
                                               vals.data(), vals.size(), &vu, &filled);
    if (s != RIFT_OK) return out;
    if (filled == 0) return out;
    size_t ko = 0, vo = 0;
    for (size_t i = 0; i < filled; ++i) {
      out.emplace_back(std::string(keys.data() + ko, klen[i]),
                       std::string(vals.data() + vo, vlen[i]));
      ko += klen[i];
      vo += vlen[i];
    }
  }
}

// THE BLOCK SIZE IS A PARAMETER AND THE ANSWER MUST NOT DEPEND ON IT. A block
// interface whose contents change with the block size is one whose amortisation
// cannot be measured, because every measurement would be of a different thing.
TEST(CApi, TheBlockSizeChangesNothingButTheNumberOfCalls) {
  Db d("blocks");
  rift_batch* b = rift_batch_new();
  for (int i = 0; i < 50; ++i) {
    char k[16];
    std::snprintf(k, sizeof k, "k%03d", i);
    ASSERT_EQ(RIFT_OK, rift_batch_set(b, k, std::strlen(k), "v", 1));
  }
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, b, &seq));
  rift_batch_free(b);

  std::vector<std::pair<std::string, std::string>> reference;
  for (size_t block : {size_t{1}, size_t{2}, size_t{7}, size_t{50}, size_t{500}}) {
    rift_iter* it = nullptr;
    ASSERT_EQ(RIFT_OK, rift_db_iter(d.db, nullptr, 0, nullptr, 0, &it));
    const auto got = Drain(it, block);
    rift_iter_free(it);
    ASSERT_EQ(50u, got.size()) << "block " << block;
    if (reference.empty()) reference = got;
    EXPECT_EQ(reference, got) << "block size " << block << " changed the ANSWER";
  }
}

// A PAIR THAT DOES NOT FIT IS HELD, NOT DROPPED. The alternative -- a short
// block with the pair lost -- makes the iterator silently skip exactly when a
// caller's buffer is tight, which is the least visible way to lose data.
TEST(CApi, APairThatDoesNotFitIsHeldForTheNextCall) {
  Db d("held");
  rift_batch* b = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "a", 1, std::string(30, 'x').data(), 30));
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "b", 1, std::string(30, 'y').data(), 30));
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "c", 1, std::string(30, 'z').data(), 30));
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, b, &seq));
  rift_batch_free(b);

  rift_iter* it = nullptr;
  ASSERT_EQ(RIFT_OK, rift_db_iter(d.db, nullptr, 0, nullptr, 0, &it));
  // A value buffer that holds two values but not three.
  const auto got = Drain(it, 10, 4096, 70);
  rift_iter_free(it);
  ASSERT_EQ(3u, got.size()) << "a pair was dropped when the buffer filled";
  EXPECT_EQ("a", got[0].first);
  EXPECT_EQ("b", got[1].first);
  EXPECT_EQ("c", got[2].first);
}

// ---------------------------------------------------------------- durability

TEST(CApi, SyncReportsAWatermarkAndDurableSeqAgrees) {
  Db d("sync");
  rift_batch* b = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "a", 1, "1", 1));
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, b, &seq));
  rift_batch_free(b);
  uint64_t w = 0;
  ASSERT_EQ(RIFT_OK, rift_db_sync(d.db, &w));
  EXPECT_EQ(seq, w);
  EXPECT_EQ(w, rift_db_durable_seq(d.db));
}

TEST(CApi, ASnapshotHoldsItsVersionAcrossTheBoundary) {
  Db d("snap");
  rift_batch* b = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_set(b, "k", 1, "before", 6));
  uint64_t seq = 0;
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, b, &seq));
  rift_batch_free(b);

  rift_snapshot* s = nullptr;
  ASSERT_EQ(RIFT_OK, rift_db_snapshot(d.db, &s));

  rift_batch* after = rift_batch_new();
  ASSERT_EQ(RIFT_OK, rift_batch_set(after, "k", 1, "after", 5));
  ASSERT_EQ(RIFT_OK, rift_db_write(d.db, after, &seq));
  rift_batch_free(after);

  char buf[64];
  size_t needed = 0;
  ASSERT_EQ(RIFT_OK, rift_snapshot_get(s, "k", 1, buf, sizeof buf, &needed));
  EXPECT_EQ("before", std::string(buf, needed));
  EXPECT_EQ("after", Get(d.db, "k"));
  EXPECT_EQ(RIFT_OK, rift_snapshot_close(s));
}

// ---------------------------------------------------------------- the guards

TEST(CApi, NullHandlesAreRefusedRatherThanDereferenced) {
  uint64_t x = 0;
  char buf[8];
  size_t n = 0;
  EXPECT_EQ(RIFT_INVALID_ARGUMENT, rift_db_sync(nullptr, &x));
  EXPECT_EQ(RIFT_INVALID_ARGUMENT, rift_db_write(nullptr, nullptr, &x));
  EXPECT_EQ(RIFT_INVALID_ARGUMENT, rift_db_get(nullptr, "k", 1, buf, sizeof buf, &n));
  EXPECT_EQ(RIFT_INVALID_ARGUMENT, rift_batch_set(nullptr, "k", 1, "v", 1));
  EXPECT_EQ(RIFT_INVALID_ARGUMENT, rift_db_close(nullptr));
  EXPECT_EQ(0u, rift_db_durable_seq(nullptr));
}

// THE BOUNDARY CANNOT THROW, AND THIS ASSERTS THE CODE ROUND-TRIPS -- NEVER
// THAT AN EXCEPTION WAS CAUGHT, which would be a claim about a mechanism this
// build does not have. The archive is compiled `-fno-exceptions`; see
// DESIGN-B5 section 2.1 and cpp-scan part 7.
TEST(CApi, TheBoundaryReportsInternalWithoutAnExceptionMechanism) {
  EXPECT_EQ(RIFT_INTERNAL, rift_test_throw());
}

}  // namespace
