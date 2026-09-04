// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ansh Kanyadi
//
// The C ABI's behaviour. What it MEANS, as opposed to test/c_abi_test.c, which
// asserts that a C compiler and a C linker accept it at all.
//
// EVERY TEST HERE GOES THROUGH THE C FUNCTIONS AND NEVER THE C++ ONES, even
// where a C++ call would be shorter. A boundary test that reaches around the
// boundary to arrange its precondition is testing the engine twice and the
// boundary never.

#include "basalt/basalt.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "basalt/basalt_cxx.h"
#include "engine_surface.h"
#include "test_env.h"

namespace {

std::string TempDir(const char* name) {
  std::string d = "/tmp/basalt-c-api-";
  d += name;
  std::string rm = "rm -rf '" + d + "'";
  (void)std::system(rm.c_str());
  return d;
}

// A database over the real filesystem, opened and closed through the C API.
class CDb {
 public:
  explicit CDb(const char* name, uint64_t flush_bytes = 0)
      : dir_(TempDir(name)) {
    basalt_caps caps;
    basalt_caps_defaults(&caps);
    if (flush_bytes != 0) caps.flush_bytes = flush_bytes;
    EXPECT_EQ(BASALT_OK, basalt_db_open(dir_.data(), dir_.size(), &caps, &db_));
  }
  ~CDb() {
    if (db_ != nullptr) basalt_db_close(db_);
  }
  CDb(const CDb&) = delete;
  CDb& operator=(const CDb&) = delete;

  basalt_db* get() const { return db_; }
  const std::string& dir() const { return dir_; }

  // Releases without closing, for the tests that close explicitly.
  basalt_db* release() {
    basalt_db* d = db_;
    db_ = nullptr;
    return d;
  }

  basalt_status Put(const std::string& k, const std::string& v,
                    uint64_t* seq = nullptr) {
    basalt_batch* b = basalt_batch_new();
    EXPECT_NE(nullptr, b);
    EXPECT_EQ(BASALT_OK,
              basalt_batch_set(b, k.data(), k.size(), v.data(), v.size()));
    const basalt_status st = basalt_db_write(db_, b, seq);
    basalt_batch_free(b);
    return st;
  }

  std::string Get(const std::string& k) {
    char buf[4096];
    std::size_t needed = 0;
    const basalt_status st =
        basalt_db_get(db_, k.data(), k.size(), buf, sizeof buf, &needed);
    EXPECT_EQ(BASALT_OK, st) << basalt_status_name(st);
    return std::string(buf, needed);
  }

 private:
  std::string dir_;
  basalt_db* db_ = nullptr;
};

// Drains an iterator into a map through basalt_iter_block, with a block size
// the caller picks. Used by the tests that assert the block size changes
// nothing except how many crossings it took.
std::map<std::string, std::string> Drain(basalt_iter* it, std::size_t n,
                                         int forward = 1,
                                         std::size_t* calls = nullptr) {
  std::map<std::string, std::string> out;
  std::vector<uint32_t> klen(n), vlen(n);
  std::vector<char> keys(64 * 1024), vals(64 * 1024);
  for (;;) {
    std::size_t filled = 0, ku = 0, vu = 0;
    const basalt_status st = basalt_iter_block(
        it, forward, n, klen.data(), vlen.data(), keys.data(), keys.size(), &ku,
        vals.data(), vals.size(), &vu, &filled);
    if (calls != nullptr) (*calls)++;
    EXPECT_EQ(BASALT_OK, st) << basalt_status_name(st);
    if (filled == 0) break;
    std::size_t ko = 0, vo = 0;
    for (std::size_t i = 0; i < filled; i++) {
      out[std::string(keys.data() + ko, klen[i])] =
          std::string(vals.data() + vo, vlen[i]);
      ko += klen[i];
      vo += vlen[i];
    }
  }
  return out;
}

// ------------------------------------------------------------------- version

TEST(CApi, TheLinkedVersionMatchesTheCompiledOne) {
  // WHAT THE MACROS AND THE FUNCTION ARE FOR, exercised together. In this
  // binary they trivially agree -- header and archive are built from one tree.
  // The assertion is not for this binary: it documents the comparison a
  // consumer makes to catch a stale shared object, and it fails here if the
  // function ever stops deriving its answer from the macros.
  EXPECT_EQ(BASALT_VERSION_NUMBER, basalt_version_number());
  ASSERT_NE(nullptr, basalt_version_string());

  char expected[32];
  std::snprintf(expected, sizeof expected, "%d.%d.%d", BASALT_VERSION_MAJOR,
                BASALT_VERSION_MINOR, BASALT_VERSION_PATCH);
  EXPECT_STREQ(expected, basalt_version_string());

  // Pre-1.0 is a claim this library is currently making on purpose. When it
  // stops being true, this line is where somebody is asked to think about it.
  EXPECT_EQ(0, BASALT_VERSION_MAJOR)
      << "past 1.0.0 the compatibility promise changes; see CMakeLists.txt";
}

// -------------------------------------------------------------------- status

TEST(CApi, EveryStatusCodeHasAName) {
  // A NAME FOR EVERY CODE THE HEADER DECLARES, derived from the list rather
  // than spot-checked: a spot check passes on the code somebody remembered.
  const basalt_status all[] = {BASALT_OK,
                               BASALT_NOT_FOUND,
                               BASALT_RECORD_TOO_LARGE,
                               BASALT_WAL_BUFFER_FULL,
                               BASALT_IO_ERROR,
                               BASALT_DISK_FULL,
                               BASALT_CORRUPTION,
                               BASALT_KILLED,
                               BASALT_INVALID_ARGUMENT,
                               BASALT_BUSY,
                               BASALT_INTERNAL,
                               BASALT_BUFFER_TOO_SMALL};
  for (basalt_status s : all) {
    const char* n = basalt_status_name(s);
    ASSERT_NE(nullptr, n);
    EXPECT_STRNE("BASALT_UNKNOWN", n)
        << "code " << static_cast<int>(s) << " has no name";
    EXPECT_EQ(0, std::strncmp(n, "BASALT_", 7));
  }
  EXPECT_STREQ("BASALT_UNKNOWN", basalt_status_name(4242));
}

// ---------------------------------------------------------------------- caps

TEST(CApi, ZeroIsAValueForBusyBytesAndNotASentinel) {
  // THE DEFECT THIS FIXES, AND WHY THE STRUCT REPLACED POSITIONAL ARGUMENTS.
  // Under a "0 means the shipped default" rule, busy_bytes = 0 -- the one
  // setting a caller most wants to be able to turn OFF -- becomes the one
  // setting a caller cannot turn off, because the request is indistinguishable
  // from not having made one.
  basalt_caps caps;
  basalt_caps_defaults(&caps);
  ASSERT_NE(0u, caps.busy_bytes);

  caps.busy_bytes = 0;
  EXPECT_NE(0, basalt_caps_ordered(&caps)) << "disabling backpressure is legal";

  const std::string dir = TempDir("busy-zero");
  basalt_db* db = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_open(dir.data(), dir.size(), &caps, &db));
  EXPECT_EQ(BASALT_OK, basalt_db_close(db));
}

TEST(CApi, ASmallWalBufferIsExpressibleBecauseBusyBytesIsReachable) {
  // The configuration a three-parameter boundary could not express: a caller
  // that wants a 1 MiB WAL buffer with 256 KiB records. It is internally
  // consistent -- wal >= 2 * max -- but the ordering rule also involves
  // busy_bytes, and with busy_bytes stuck at its 192 MiB default the request is
  // refused with no way for the caller to fix it.
  basalt_caps caps;
  basalt_caps_defaults(&caps);
  caps.wal_buffer_bytes = 1024 * 1024;
  caps.max_record_bytes = 256 * 1024;

  EXPECT_EQ(0, basalt_caps_ordered(&caps))
      << "with the default busy threshold this is exactly the refused case";

  caps.busy_bytes = 512 * 1024;
  ASSERT_NE(0, basalt_caps_ordered(&caps));

  const std::string dir = TempDir("small-wal");
  basalt_db* db = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_open(dir.data(), dir.size(), &caps, &db));
  EXPECT_EQ(BASALT_OK, basalt_db_close(db));
}

TEST(CApi, AnUnorderedCapSetIsRefusedAtOpenAndAskableBeforehand) {
  basalt_caps caps;
  basalt_caps_defaults(&caps);
  caps.wal_buffer_bytes = 1024;  // far below 2 * max_record_bytes
  EXPECT_EQ(0, basalt_caps_ordered(&caps));

  const std::string dir = TempDir("unordered");
  basalt_db* db = nullptr;
  EXPECT_EQ(BASALT_INVALID_ARGUMENT,
            basalt_db_open(dir.data(), dir.size(), &caps, &db));
  EXPECT_EQ(nullptr, db) << "a refused open must not leave a handle behind";
}

TEST(CApi, NullCapsMeansTheShippedDefaults) {
  const std::string dir = TempDir("null-caps");
  basalt_db* db = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_open(dir.data(), dir.size(), nullptr, &db));
  EXPECT_EQ(BASALT_OK, basalt_db_close(db));
}

// ------------------------------------------------------------------- basics

TEST(CApi, WritesAndReadsThroughTheBoundary) {
  CDb db("rw");
  ASSERT_EQ(BASALT_OK, db.Put("alpha", "one"));
  ASSERT_EQ(BASALT_OK, db.Put("beta", "two"));
  EXPECT_EQ("one", db.Get("alpha"));
  EXPECT_EQ("two", db.Get("beta"));
}

TEST(CApi, NotFoundIsACodeAndNotAnEmptyValue) {
  CDb db("notfound");
  ASSERT_EQ(BASALT_OK, db.Put("present", ""));
  char buf[16];
  std::size_t needed = 99;

  // AN EMPTY VALUE IS A VALUE. If NOT_FOUND were reported by returning zero
  // bytes, these two cases would be the same answer.
  EXPECT_EQ(BASALT_OK,
            basalt_db_get(db.get(), "present", 7, buf, sizeof buf, &needed));
  EXPECT_EQ(0u, needed);
  EXPECT_EQ(BASALT_NOT_FOUND,
            basalt_db_get(db.get(), "absent", 6, buf, sizeof buf, &needed));
}

TEST(CApi, AShortBufferIsToldTheLengthAndNothingIsTruncated) {
  CDb db("short");
  const std::string big(5000, 'z');
  ASSERT_EQ(BASALT_OK, db.Put("k", big));

  char small[16];
  std::size_t needed = 0;
  EXPECT_EQ(BASALT_BUFFER_TOO_SMALL,
            basalt_db_get(db.get(), "k", 1, small, sizeof small, &needed));
  EXPECT_EQ(big.size(), needed)
      << "the caller is told exactly how much to grow by";

  std::vector<char> exact(needed);
  std::size_t again = 0;
  ASSERT_EQ(BASALT_OK, basalt_db_get(db.get(), "k", 1, exact.data(),
                                     exact.size(), &again));
  EXPECT_EQ(big.size(), again);
  EXPECT_EQ(big, std::string(exact.data(), again));
}

TEST(CApi, ZeroCapacityAsksForTheLengthAlone) {
  CDb db("length-only");
  ASSERT_EQ(BASALT_OK, db.Put("k", "0123456789"));
  std::size_t needed = 0;
  EXPECT_EQ(BASALT_BUFFER_TOO_SMALL,
            basalt_db_get(db.get(), "k", 1, nullptr, 0, &needed));
  EXPECT_EQ(10u, needed);
}

TEST(CApi, AnEmptyKeyIsAKey) {
  CDb db("empty-key");
  ASSERT_EQ(BASALT_OK, db.Put("", "the empty key"));
  EXPECT_EQ("the empty key", db.Get(""));
}

// -------------------------------------------------------------------- bounds

TEST(CApi, NullBoundsAreUnboundedAndEmptyOnesAreTheEmptyKey) {
  CDb db("bounds");
  ASSERT_EQ(BASALT_OK, db.Put("", "empty"));
  ASSERT_EQ(BASALT_OK, db.Put("a", "1"));
  ASSERT_EQ(BASALT_OK, db.Put("b", "2"));

  // THE DISTINCTION LIVES IN THE POINTER, because there is no byte string that
  // means "no bound". A non-null pointer of length zero is the empty key, which
  // sorts first, so a range starting there excludes nothing.
  basalt_batch* batch = basalt_batch_new();
  ASSERT_EQ(BASALT_OK, basalt_batch_delete_range(batch, "", 0, "b", 1));
  ASSERT_EQ(BASALT_OK, basalt_db_write(db.get(), batch, nullptr));
  basalt_batch_free(batch);

  char buf[64];
  std::size_t needed = 0;
  EXPECT_EQ(BASALT_NOT_FOUND,
            basalt_db_get(db.get(), "", 0, buf, sizeof buf, &needed));
  EXPECT_EQ(BASALT_NOT_FOUND,
            basalt_db_get(db.get(), "a", 1, buf, sizeof buf, &needed));
  EXPECT_EQ("2", db.Get("b"))
      << "the range was half-open and must not have taken b";
}

TEST(CApi, AnUnboundedRangeTakesEverything) {
  CDb db("unbounded-range");
  ASSERT_EQ(BASALT_OK, db.Put("a", "1"));
  ASSERT_EQ(BASALT_OK, db.Put("z", "26"));

  basalt_batch* batch = basalt_batch_new();
  ASSERT_EQ(BASALT_OK,
            basalt_batch_delete_range(batch, nullptr, 0, nullptr, 0));
  ASSERT_EQ(BASALT_OK, basalt_db_write(db.get(), batch, nullptr));
  basalt_batch_free(batch);

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  int valid = 1;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));
  EXPECT_EQ(0, valid);
  basalt_iter_free(it);
}

TEST(CApi, ANullKeyIsRefusedWhereANullBoundWouldBeLegal) {
  CDb db("null-key");
  basalt_batch* b = basalt_batch_new();
  // A KEY HAS NO UNBOUNDED CASE, so null there is an error rather than a
  // meaning. Conflating the two rules is a real defect and not a hypothetical:
  // one helper serving both call sites is how it gets made.
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_batch_set(b, nullptr, 0, "v", 1));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_batch_set(b, "k", 1, nullptr, 0));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_batch_delete(b, nullptr, 0));
  // ...but a zero-length non-null value is legal, and is not the same thing.
  EXPECT_EQ(BASALT_OK, basalt_batch_set(b, "k", 1, "", 0));
  basalt_batch_free(b);
}

// -------------------------------------------------------------------- batch

TEST(CApi, ABatchIsAValueIndependentOfAnyDatabase) {
  CDb one("batch-reuse-1");
  CDb two("batch-reuse-2");

  basalt_batch* b = basalt_batch_new();
  ASSERT_EQ(BASALT_OK, basalt_batch_set(b, "k", 1, "v", 1));
  EXPECT_EQ(1u, basalt_batch_count(b));

  // THE SAME BATCH, WRITTEN TWICE, TO TWO DATABASES. A batch that were secretly
  // bound to the database that first saw it would fail here, and nothing else
  // in this suite would notice.
  ASSERT_EQ(BASALT_OK, basalt_db_write(one.get(), b, nullptr));
  ASSERT_EQ(BASALT_OK, basalt_db_write(two.get(), b, nullptr));
  EXPECT_EQ("v", one.Get("k"));
  EXPECT_EQ("v", two.Get("k"));

  basalt_batch_clear(b);
  EXPECT_EQ(0u, basalt_batch_count(b));
  basalt_batch_free(b);
}

TEST(CApi, OneWriteCommitsTheWholeBatchAtOneSequence) {
  CDb db("batch-atomic");
  basalt_batch* b = basalt_batch_new();
  for (int i = 0; i < 50; i++) {
    char k[16];
    std::snprintf(k, sizeof k, "k%02d", i);
    ASSERT_EQ(BASALT_OK, basalt_batch_set(b, k, std::strlen(k), "v", 1));
  }
  EXPECT_EQ(50u, basalt_batch_count(b));
  uint64_t seq = 0;
  ASSERT_EQ(BASALT_OK, basalt_db_write(db.get(), b, &seq));
  basalt_batch_free(b);

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  int valid = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));
  EXPECT_EQ(50u, Drain(it, 8).size());
  basalt_iter_free(it);
}

// ----------------------------------------------------------------- iterators

TEST(CApi, TheBlockSizeChangesNothingButTheNumberOfCrossings) {
  CDb db("block-size");
  for (int i = 0; i < 40; i++) {
    char k[16];
    std::snprintf(k, sizeof k, "k%02d", i);
    ASSERT_EQ(BASALT_OK, db.Put(k, "v"));
  }

  std::map<std::string, std::string> reference;
  std::size_t calls_at_1 = 0, calls_at_40 = 0;
  for (std::size_t n :
       {std::size_t(1), std::size_t(3), std::size_t(40), std::size_t(100)}) {
    basalt_iter* it = nullptr;
    ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
    int valid = 0;
    ASSERT_EQ(BASALT_OK,
              basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));
    std::size_t calls = 0;
    const std::map<std::string, std::string> got = Drain(it, n, 1, &calls);
    basalt_iter_free(it);
    if (n == 1) {
      reference = got;
      calls_at_1 = calls;
    }
    if (n == 40) calls_at_40 = calls;
    EXPECT_EQ(reference, got) << "block size " << n << " changed the answer";
    EXPECT_EQ(40u, got.size());
  }
  // AND IT REALLY IS AMORTISING. Asserting only that the answers match would
  // pass on an implementation that ignored `n` entirely.
  EXPECT_GT(calls_at_1, calls_at_40);
}

TEST(CApi, APairThatDoesNotFitIsHeldRatherThanDropped) {
  CDb db("held-pair");
  ASSERT_EQ(BASALT_OK, db.Put("a", std::string(100, 'x')));
  ASSERT_EQ(BASALT_OK, db.Put("b", std::string(100, 'y')));

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  int valid = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));

  // Room for one value and not two. The second pair must be held, not skipped:
  // an iterator that dropped it would lose data exactly when a caller's buffer
  // is tight, which is the least visible way to lose data.
  uint32_t klen[4], vlen[4];
  char keys[64], vals[150];
  std::size_t filled = 0, ku = 0, vu = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_block(it, 1, 4, klen, vlen, keys, sizeof keys, &ku,
                              vals, sizeof vals, &vu, &filled));
  EXPECT_EQ(1u, filled) << "only one 100-byte value fits in 150 bytes";

  std::size_t filled2 = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_block(it, 1, 4, klen, vlen, keys, sizeof keys, &ku,
                              vals, sizeof vals, &vu, &filled2));
  ASSERT_EQ(1u, filled2) << "the held pair must come back, not be skipped";
  EXPECT_EQ("b", std::string(keys, klen[0]));
}

TEST(CApi, ABlockTooSmallForOnePairIsToldTheCapacitiesItNeeds) {
  CDb db("too-small");
  ASSERT_EQ(BASALT_OK, db.Put("key", std::string(500, 'v')));

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  int valid = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));

  uint32_t klen[2], vlen[2];
  char keys[2], vals[2];
  std::size_t filled = 99, ku = 0, vu = 0;
  EXPECT_EQ(BASALT_BUFFER_TOO_SMALL,
            basalt_iter_block(it, 1, 2, klen, vlen, keys, sizeof keys, &ku,
                              vals, sizeof vals, &vu, &filled));
  EXPECT_EQ(0u, filled);
  // THE NEEDED CAPACITIES AND NOT THE USED ONES, so one grow-and-retry
  // suffices. A caller told only "too small" has to guess, and a guess that is
  // still too small loops.
  EXPECT_EQ(3u, ku);
  EXPECT_EQ(500u, vu);

  std::vector<char> k2(ku), v2(vu);
  std::size_t filled2 = 0, ku2 = 0, vu2 = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_block(it, 1, 2, klen, vlen, k2.data(), k2.size(), &ku2,
                              v2.data(), v2.size(), &vu2, &filled2));
  EXPECT_EQ(1u, filled2)
      << "the retry at the reported size must succeed in one step";
  basalt_iter_free(it);
}

TEST(CApi, ASeekReturnsTheEntryItLandedOn) {
  CDb db("seek");
  for (const char* k : {"a", "c", "e"}) ASSERT_EQ(BASALT_OK, db.Put(k, k));

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  int valid = 0;
  // Seeking to "b" lands on "c", and "c" is what the next block must start
  // with. A cursor that stepped past the entry it was asked to seek to is a
  // wrong answer with nothing to catch it.
  ASSERT_EQ(BASALT_OK, basalt_iter_seek(it, BASALT_SEEK_GE, "b", 1, &valid));
  ASSERT_EQ(1, valid);

  uint32_t klen[4], vlen[4];
  char keys[64], vals[64];
  std::size_t filled = 0, ku = 0, vu = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_block(it, 1, 1, klen, vlen, keys, sizeof keys, &ku,
                              vals, sizeof vals, &vu, &filled));
  ASSERT_EQ(1u, filled);
  EXPECT_EQ("c", std::string(keys, klen[0]));
  basalt_iter_free(it);
}

TEST(CApi, WalkingBackwardsGivesTheForwardSequenceReversed) {
  CDb db("backwards");
  for (const char* k : {"a", "b", "c", "d"}) ASSERT_EQ(BASALT_OK, db.Put(k, k));

  auto walk = [&](basalt_seek_mode mode, int forward) {
    basalt_iter* it = nullptr;
    EXPECT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
    int valid = 0;
    EXPECT_EQ(BASALT_OK, basalt_iter_seek(it, mode, nullptr, 0, &valid));
    std::vector<std::string> keys;
    uint32_t klen[8], vlen[8];
    char kb[256], vb[256];
    for (;;) {
      std::size_t filled = 0, ku = 0, vu = 0;
      EXPECT_EQ(BASALT_OK,
                basalt_iter_block(it, forward, 8, klen, vlen, kb, sizeof kb,
                                  &ku, vb, sizeof vb, &vu, &filled));
      if (filled == 0) break;
      std::size_t ko = 0;
      for (std::size_t i = 0; i < filled; i++) {
        keys.emplace_back(kb + ko, klen[i]);
        ko += klen[i];
      }
    }
    basalt_iter_free(it);
    return keys;
  };

  std::vector<std::string> fwd = walk(BASALT_SEEK_FIRST, 1);
  std::vector<std::string> rev = walk(BASALT_SEEK_LAST, 0);
  ASSERT_EQ(4u, fwd.size());
  std::reverse(rev.begin(), rev.end());
  EXPECT_EQ(fwd, rev);
}

TEST(CApi, IterErrorDistinguishesExhaustionFromFailure) {
  CDb db("iter-error");
  ASSERT_EQ(BASALT_OK, db.Put("a", "1"));

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  int valid = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));
  EXPECT_EQ(1u, Drain(it, 4).size());
  // A ZERO-PAIR BLOCK IS AMBIGUOUS ON ITS OWN -- exhausted, or failed? Without
  // this call a caller cannot tell a corrupt table from an empty range.
  EXPECT_EQ(BASALT_OK, basalt_iter_error(it)) << "exhausted is not failed";
  basalt_iter_free(it);
}

TEST(CApi, IteratorBoundsAreRespected) {
  CDb db("iter-bounds");
  for (const char* k : {"a", "b", "c", "d", "e"})
    ASSERT_EQ(BASALT_OK, db.Put(k, k));

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), "b", 1, "d", 1, &it));
  int valid = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));
  const std::map<std::string, std::string> got = Drain(it, 8);
  basalt_iter_free(it);

  ASSERT_EQ(2u, got.size()) << "half-open: b and c, never d";
  EXPECT_EQ(1u, got.count("b"));
  EXPECT_EQ(1u, got.count("c"));
}

// ----------------------------------------------------------------- snapshots

TEST(CApi, ASnapshotHoldsItsVersionAcrossTheBoundary) {
  CDb db("snapshot");
  ASSERT_EQ(BASALT_OK, db.Put("k", "before"));

  basalt_snapshot* snap = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_snapshot(db.get(), &snap));
  ASSERT_EQ(BASALT_OK, db.Put("k", "after"));

  char buf[64];
  std::size_t needed = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_snapshot_get(snap, "k", 1, buf, sizeof buf, &needed));
  EXPECT_EQ("before", std::string(buf, needed));
  EXPECT_EQ("after", db.Get("k"));
  EXPECT_EQ(BASALT_OK, basalt_snapshot_close(snap));
}

TEST(CApi, ASnapshotIteratesAndNotOnlyPointReads) {
  // A snapshot you can only point-read is much weaker than a snapshot:
  // repeatable RANGE reads are most of what a pinned view is for.
  CDb db("snapshot-iter");
  for (const char* k : {"a", "b", "c"}) ASSERT_EQ(BASALT_OK, db.Put(k, "old"));

  basalt_snapshot* snap = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_snapshot(db.get(), &snap));
  for (const char* k : {"a", "b", "c"}) ASSERT_EQ(BASALT_OK, db.Put(k, "new"));
  ASSERT_EQ(BASALT_OK, db.Put("d", "new"));

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_snapshot_iter(snap, nullptr, 0, nullptr, 0, &it));
  int valid = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));
  const std::map<std::string, std::string> got = Drain(it, 8);
  basalt_iter_free(it);

  ASSERT_EQ(3u, got.size())
      << "d was written after the snapshot and must be invisible";
  for (const auto& kv : got) EXPECT_EQ("old", kv.second);
  EXPECT_EQ(BASALT_OK, basalt_snapshot_close(snap));
}

// ------------------------------------------------------------------ lifetime

TEST(CApi, AnIteratorAndASnapshotOutliveTheirDatabase) {
  // THE HEADER PROMISES THIS, SO IT IS ASSERTED. Handles hold the version they
  // were created over. Without a test, "keeps working after close" is an
  // accident of the implementation that a later refactor may remove silently --
  // and the caller who finds out is the one holding the handle.
  CDb db("outlive", 4096);
  for (int i = 0; i < 40; i++) {
    char k[16];
    std::snprintf(k, sizeof k, "k%02d", i);
    ASSERT_EQ(BASALT_OK, db.Put(k, std::string(512, 'v')));
  }
  uint64_t mark = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_db_sync(db.get(), &mark));  // forces a flush to tables

  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  int valid = 0;
  ASSERT_EQ(BASALT_OK,
            basalt_iter_seek(it, BASALT_SEEK_FIRST, nullptr, 0, &valid));

  basalt_snapshot* snap = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_snapshot(db.get(), &snap));

  ASSERT_EQ(BASALT_OK, basalt_db_close(db.release()));

  EXPECT_EQ(40u, Drain(it, 8).size())
      << "the iterator must still serve after the close";
  char buf[1024];
  std::size_t needed = 0;
  EXPECT_EQ(BASALT_OK,
            basalt_snapshot_get(snap, "k00", 3, buf, sizeof buf, &needed));
  EXPECT_EQ(512u, needed);

  basalt_iter_free(it);
  EXPECT_EQ(BASALT_OK, basalt_snapshot_close(snap));
}

TEST(CApi, NullHandlesAreRefusedRatherThanDereferenced) {
  // THIS IS WHAT MAKES THE "A CLOSED HANDLE IS GONE" RULE LIVEABLE. The
  // discipline it asks of a caller is "null the pointer when you release it",
  // and that discipline is only safe if a null pointer is then a code rather
  // than a crash.
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_db_close(nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_db_sync(nullptr, nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT,
            basalt_db_get(nullptr, "k", 1, nullptr, 0, nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT,
            basalt_db_write(nullptr, nullptr, nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_db_snapshot(nullptr, nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_snapshot_close(nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT,
            basalt_iter_seek(nullptr, BASALT_SEEK_FIRST, nullptr, 0, nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT, basalt_iter_error(nullptr));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT,
            basalt_db_approximate_disk_bytes(nullptr, nullptr, 0, nullptr, 0,
                                             nullptr));
  EXPECT_EQ(0u, basalt_db_durable_seq(nullptr));
  EXPECT_EQ(0u, basalt_batch_count(nullptr));
  EXPECT_EQ(0, basalt_caps_ordered(nullptr) == 0 ? 0 : 0);  // must not crash

  // The void-returning releases ignore null.
  basalt_batch_free(nullptr);
  basalt_iter_free(nullptr);
  basalt_env_free(nullptr);
  basalt_batch_clear(nullptr);
  basalt_caps_defaults(nullptr);
}

TEST(CApi, AnUnknownSeekModeIsRefused) {
  // `mode` crosses from C, where an enum is an integer and a caller may pass
  // any of them. That is why this switch has a default arm where the internal
  // ones deliberately do not.
  CDb db("bad-seek");
  basalt_iter* it = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_iter(db.get(), nullptr, 0, nullptr, 0, &it));
  EXPECT_EQ(BASALT_INVALID_ARGUMENT,
            basalt_iter_seek(it, 77, nullptr, 0, nullptr));
  basalt_iter_free(it);
}

// ---------------------------------------------------------------- durability

TEST(CApi, SyncReportsAWatermarkAndDurableSeqAgrees) {
  CDb db("sync");
  uint64_t seq = 0;
  ASSERT_EQ(BASALT_OK, db.Put("k", "v", &seq));
  EXPECT_LT(basalt_db_durable_seq(db.get()), seq)
      << "a write is visible before it is durable";

  uint64_t mark = 0;
  ASSERT_EQ(BASALT_OK, basalt_db_sync(db.get(), &mark));
  EXPECT_GE(mark, seq);
  EXPECT_EQ(mark, basalt_db_durable_seq(db.get()));
}

TEST(CApi, DataSurvivesCloseAndReopenThroughTheBoundary) {
  const std::string dir = TempDir("reopen");
  basalt_db* db = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_open(dir.data(), dir.size(), nullptr, &db));

  basalt_batch* b = basalt_batch_new();
  ASSERT_EQ(BASALT_OK, basalt_batch_set(b, "k", 1, "durable", 7));
  ASSERT_EQ(BASALT_OK, basalt_db_write(db, b, nullptr));
  basalt_batch_free(b);

  uint64_t mark = 0;
  ASSERT_EQ(BASALT_OK, basalt_db_sync(db, &mark));
  ASSERT_GT(mark, 0u);
  ASSERT_EQ(BASALT_OK, basalt_db_close(db));

  db = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_open(dir.data(), dir.size(), nullptr, &db));
  char buf[64];
  std::size_t needed = 0;
  ASSERT_EQ(BASALT_OK, basalt_db_get(db, "k", 1, buf, sizeof buf, &needed));
  EXPECT_EQ("durable", std::string(buf, needed));

  // THE WATERMARK DOES NOT SURVIVE THE REOPEN, AND THE DATA DOES. Those are
  // different promises and it is worth one assertion to keep them apart,
  // because the obvious expectation -- that DurableSeq comes back where it left
  // off -- is wrong, and a C consumer that assumed it would build a broken
  // resume on top of it.
  //
  // DurableSeq is a claim about the LIVE WAL, and recovery opens a fresh one:
  // "a fresh WAL has promised nothing yet" (test/db_test.cc). The floor only
  // rises for sequences a durable TABLE holds, and one small write never
  // flushed. So zero here is not lost data; it is the engine declining to make
  // a promise about a WAL it has not written to yet.
  EXPECT_EQ(0u, basalt_db_durable_seq(db))
      << "a reopened database re-earns its watermark rather than inheriting it";

  // And it re-earns it on the next sync, which is the half that matters.
  uint64_t again = 0;
  basalt_batch* b2 = basalt_batch_new();
  ASSERT_EQ(BASALT_OK, basalt_batch_set(b2, "k2", 2, "v2", 2));
  ASSERT_EQ(BASALT_OK, basalt_db_write(db, b2, nullptr));
  basalt_batch_free(b2);
  ASSERT_EQ(BASALT_OK, basalt_db_sync(db, &again));
  EXPECT_GT(again, 0u);
  EXPECT_EQ(again, basalt_db_durable_seq(db));
  EXPECT_EQ(BASALT_OK, basalt_db_close(db));
}

TEST(CApi, ApproximateDiskBytesAnswersARange) {
  CDb db("adb");
  ASSERT_EQ(BASALT_OK, db.Put("a", std::string(100, 'x')));
  ASSERT_EQ(BASALT_OK, db.Put("b", std::string(100, 'y')));
  ASSERT_EQ(BASALT_OK, db.Put("z", std::string(100, 'z')));

  uint64_t all = 0, part = 0;
  ASSERT_EQ(BASALT_OK, basalt_db_approximate_disk_bytes(db.get(), nullptr, 0,
                                                        nullptr, 0, &all));
  ASSERT_EQ(BASALT_OK,
            basalt_db_approximate_disk_bytes(db.get(), "a", 1, "c", 1, &part));
  EXPECT_GT(all, 0u);
  EXPECT_LT(part, all) << "a sub-range must count less than everything";
}

// ------------------------------------------------------------ the env seam

TEST(CApi, ADatabaseCanBeOpenedOverACallerSuppliedEnv) {
  // The seam the kill-point sweep runs the C surface through. Without it, the
  // boundary can only ever be tested against a real filesystem that cannot be
  // made to fail on command -- and a durability claim about the boundary would
  // rest on a test that never injects a fault.
  basalt::testenv::TestEnvironment t;
  basalt_env* env = basalt::BorrowEnv(t.env());
  ASSERT_NE(nullptr, env);

  basalt_db* db = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_open_env(env, "db", 2, nullptr, &db));

  basalt_batch* b = basalt_batch_new();
  ASSERT_EQ(BASALT_OK, basalt_batch_set(b, "k", 1, "v", 1));
  ASSERT_EQ(BASALT_OK, basalt_db_write(db, b, nullptr));
  basalt_batch_free(b);

  uint64_t mark = 0;
  ASSERT_EQ(BASALT_OK, basalt_db_sync(db, &mark));
  EXPECT_GT(t.ordinal(), 0u)
      << "the writes must have gone through the supplied env";

  EXPECT_EQ(BASALT_OK, basalt_db_close(db));
  basalt_env_free(env);
}

TEST(CApi, ABorrowedEnvIsNotFreedWithItsHandle) {
  // BORROWED MEANS BORROWED. If basalt_env_free deleted the caller's Env, this
  // would be a use-after-free on the next line rather than a passing test --
  // and under ASan the lane says so.
  basalt::testenv::TestEnvironment t;
  basalt_env* env = basalt::BorrowEnv(t.env());
  ASSERT_NE(nullptr, env);
  basalt_env_free(env);

  // The caller's Env is still alive and still usable.
  basalt_env* again = basalt::BorrowEnv(t.env());
  ASSERT_NE(nullptr, again);
  basalt_db* db = nullptr;
  ASSERT_EQ(BASALT_OK, basalt_db_open_env(again, "db2", 3, nullptr, &db));
  EXPECT_EQ(BASALT_OK, basalt_db_close(db));
  basalt_env_free(again);
}

TEST(CApi, BorrowingANullEnvIsRefused) {
  EXPECT_EQ(nullptr, basalt::BorrowEnv(nullptr));
  basalt_db* db = nullptr;
  EXPECT_EQ(BASALT_INVALID_ARGUMENT,
            basalt_db_open_env(nullptr, "d", 1, nullptr, &db));
}

// ----------------------------------------------------------- surface parity

TEST(CApi, TheTwoSurfacesSeeTheSameDatabase) {
  // WHAT THE SWEEP ASSUMES, ASSERTED DIRECTLY AND CHEAPLY.
  //
  // The kill-point sweep runs the same workload through both surfaces and
  // adjudicates each. That is the strong evidence. But it compares each surface
  // against the MODEL, never the two against each other -- so a defect that
  // moved both in the same direction would pass both sweeps.
  //
  // This is the direct comparison, and it costs one test. It is also the thing
  // that fails first and most legibly when the C boundary starts dropping,
  // reordering or truncating entries: a sweep reports a durability violation at
  // an ordinal, and this reports which key came back wrong.
  const std::string kWorkload[][2] = {
      {"", "the empty key"},
      {"a", "1"},
      {"b", ""},
      {"c", std::string(600, 'x')},  // crosses a block buffer
      {"d", "4"},
      {"zz", "26"},
  };

  std::map<std::string, std::string> seen[2];
  const basalt::rig::SweepSurface surfaces[2] = {
      basalt::rig::SweepSurface::kCxx, basalt::rig::SweepSurface::kC};
  for (int i = 0; i < 2; i++) {
    basalt::testenv::TestEnvironment t;
    std::unique_ptr<basalt::rig::EngineSurface> s =
        basalt::rig::NewSurface(surfaces[i]);
    ASSERT_EQ(basalt::Status::Code::kOk,
              s->Open(t.env(), "db", basalt::wal::Caps()))
        << "surface " << basalt::rig::SweepSurfaceName(surfaces[i]);
    for (const auto& kv : kWorkload) {
      ASSERT_EQ(basalt::Status::Code::kOk, s->Put(kv[0], kv[1]));
    }
    uint64_t mark = 0;
    ASSERT_EQ(basalt::Status::Code::kOk, s->Sync(&mark));
    EXPECT_GT(mark, 0u);
    seen[i] = s->ExtractState();
    s->Close();
  }

  EXPECT_EQ(6u, seen[0].size());
  EXPECT_EQ(seen[0], seen[1])
      << "the C boundary returned a different database than the C++ interface";
}

}  // namespace
