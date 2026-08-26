// The manifest: a WAL-framed log, read by the reader B1.7a already induced.
//
// Nothing here re-tests the framing -- that is B2-D4(c)'s whole argument, and
// re-proving it would mean the reuse bought nothing. What IS tested is what the
// reuse does NOT give for free: which record kinds a manifest may hold, that
// its shared group terminator can carry no sequence, that every number it
// records is re-derived from the file that justifies it, and that CURRENT is
// installed by rename plus a directory sync.
#include "manifest.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "format.h"
#include "read_whole_file.h"
#include "recovery.h"
#include "table_builder.h"
#include "test_env.h"
#include "writer.h"

namespace rift {
namespace sst {
namespace {

using testenv::FaultPlan;
using testenv::Injection;
using testenv::TestEnvironment;

const std::string kDir = "db";

// The WAL fragment header is seven bytes: crc32c, length, type. Named here so
// the fixture below computes an offset rather than counting one.
constexpr uint64_t kWalFragmentHeaderBytes = 7;
constexpr uint64_t kWalBlockBytes = 32768;

std::string IKey(const std::string& user, SeqNum seq,
                 ValueType type = ValueType::kValue) {
  std::string out;
  AppendInternalKey(&out, Slice(user), MakeTag(seq, type));
  return out;
}

void PutFile(TestEnvironment* t, const std::string& path, Slice bytes) {
  WritableFilePtr f;
  ASSERT_TRUE(t->env()->NewWritableFile(path, &f).ok());
  ASSERT_TRUE(f->Append(bytes).ok());
  ASSERT_TRUE(f->Sync().ok());
  ASSERT_TRUE(f->Close().ok());
}

// Writes a real SSTable and returns what the manifest should record about it.
TableMeta WriteTable(TestEnvironment* t, uint64_t number,
                     const std::vector<std::pair<std::string, std::string>>& kvs) {
  WritableFilePtr f;
  EXPECT_TRUE(t->env()->NewWritableFile(TablePath(kDir, number), &f).ok());
  TableMeta meta;
  meta.number = number;
  {
    TableBuilder b(f.get());
    for (const auto& kv : kvs) b.Add(Slice(kv.first), Slice(kv.second));
    EXPECT_TRUE(b.Finish().ok());
    meta.file_bytes = b.file_size();
    meta.smallest = b.smallest().ToString();
    meta.largest = b.largest().ToString();
    meta.largest_seq = b.largest_seq();
  }
  EXPECT_TRUE(f->Sync().ok());
  EXPECT_TRUE(f->Close().ok());
  return meta;
}

ManifestEdit Add(const TableMeta& m) {
  ManifestEdit e;
  e.kind = EditKind::kAddTable;
  e.table = m;
  return e;
}

// ------------------------------------------------------------ open and reopen

TEST(Manifest, AFreshDirectoryGetsAManifestAndACurrent) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  ManifestState state;
  std::unique_ptr<Manifest> m;
  ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
  EXPECT_EQ(1u, m->number());
  EXPECT_TRUE(state.tables.empty());
  EXPECT_EQ(2u, state.next_file_number);
  EXPECT_EQ("MANIFEST-000001\n", t.ContentNow(CurrentPath(kDir)));
  ASSERT_TRUE(m->Close().ok());
}

TEST(Manifest, WhatIsAppendedSurvivesAReopen) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  TableMeta meta;
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    meta = WriteTable(&t, state.next_file_number,
                      {{IKey("a", 3), "1"}, {IKey("b", 7), "2"}});
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = state.next_file_number + 1;
    ASSERT_TRUE(m->AppendGroup({Add(meta), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
  ASSERT_EQ(1u, state.tables.size());
  const TableMeta& got = state.tables.begin()->second;
  EXPECT_EQ(meta.number, got.number);
  EXPECT_EQ(meta.file_bytes, got.file_bytes);
  EXPECT_EQ(meta.smallest, got.smallest);
  EXPECT_EQ(meta.largest, got.largest);
  EXPECT_EQ(7u, got.largest_seq);
  ASSERT_TRUE(m->Close().ok());
}

// ------------------------------------------------------------------- levels

TEST(Manifest, TheLevelSurvivesAReopen) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    TableMeta meta = WriteTable(&t, state.next_file_number,
                                {{IKey("a", 3), "1"}, {IKey("b", 7), "2"}});
    meta.level = 1;
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = state.next_file_number + 1;
    ASSERT_TRUE(m->AppendGroup({Add(meta), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
  ASSERT_EQ(1u, state.tables.size());
  EXPECT_EQ(1, state.tables.begin()->second.level);
  ASSERT_TRUE(m->Close().ok());
}

// REFUSED, NOT CLAMPED, and the reason is in manifest.cc: a file from a build
// with more levels placed at level 1 joins a run the read path binary searches,
// and an overlapping member of that run makes a key INVISIBLE.
TEST(Manifest, ALevelThisBuildCannotPlaceIsRefused) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    TableMeta meta = WriteTable(&t, state.next_file_number, {{IKey("a", 3), "1"}});
    meta.level = 2;
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = state.next_file_number + 1;
    ASSERT_TRUE(m->AppendGroup({Add(meta), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  const Status s = Manifest::Open(t.env(), kDir, &state, nullptr, &m);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("levels"));
}

// Two L1 files that share a user key. `L1FileFor` finds ONE of them and the
// other's versions cannot be reached -- so a deletion stops hiding a value and
// deleted data returns, with nothing structurally wrong in either file.
TEST(Manifest, AnOverlappingLevelOneIsRefused) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    TableMeta lo = WriteTable(&t, 2, {{IKey("a", 3), "1"}, {IKey("m", 4), "2"}});
    TableMeta hi = WriteTable(&t, 3, {{IKey("m", 9), "3"}, {IKey("z", 5), "4"}});
    lo.level = 1;
    hi.level = 1;
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = 4;
    ASSERT_TRUE(m->AppendGroup({Add(lo), Add(hi), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  const Status s = Manifest::Open(t.env(), kDir, &state, nullptr, &m);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("not a run"));
}

// GF-14: THE OTHER HALF. Without it the refusal above could be produced by a
// check that rejects every two-file L1, and nothing would say so.
TEST(Manifest, AdjacentLevelOneTablesAreARun) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    TableMeta lo = WriteTable(&t, 2, {{IKey("a", 3), "1"}, {IKey("m", 4), "2"}});
    TableMeta hi = WriteTable(&t, 3, {{IKey("n", 9), "3"}, {IKey("z", 5), "4"}});
    lo.level = 1;
    hi.level = 1;
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = 4;
    ASSERT_TRUE(m->AppendGroup({Add(lo), Add(hi), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
  EXPECT_EQ(2u, state.tables.size());
  ASSERT_TRUE(m->Close().ok());
}

TEST(Manifest, EveryOpenRotatesAndTheOldOneIsRemoved) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  for (uint64_t expected = 1; expected <= 3; ++expected) {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    EXPECT_EQ(expected, m->number());
    ASSERT_TRUE(m->Close().ok());
    std::vector<std::string> children;
    ASSERT_TRUE(t.env()->GetChildren(kDir, &children).ok());
    int manifests = 0;
    for (const std::string& c : children) {
      if (c.compare(0, 9, "MANIFEST-") == 0) ++manifests;
    }
    EXPECT_EQ(1, manifests) << "a retired manifest was left behind";
  }
}

// --------------------------------------- the numbers the manifest may record

TEST(Manifest, ARecordedSequenceIsHeldToTheTableThatJustifiesIt) {
  // D4 SECTION 5.1 POINT 2. The manifest is never the sole authority for any
  // number in it: Open re-derives the largest sequence from the table's own
  // keys and refuses the open on disagreement. This is the tampering test the
  // design asks for, one number over.
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    TableMeta meta = WriteTable(&t, state.next_file_number,
                                {{IKey("a", 3), "1"}, {IKey("b", 7), "2"}});
    meta.largest_seq = 99;  // a number the table cannot justify
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = state.next_file_number + 1;
    ASSERT_TRUE(m->AppendGroup({Add(meta), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  const Status s = Manifest::Open(t.env(), kDir, &state, nullptr, &m);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("99")) << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("7")) << s.ToString();
}

TEST(Manifest, ARecordedSizeIsHeldToTheFileThatJustifiesIt) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    TableMeta meta = WriteTable(&t, state.next_file_number, {{IKey("a", 3), "1"}});
    meta.file_bytes += 1;
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = state.next_file_number + 1;
    ASSERT_TRUE(m->AppendGroup({Add(meta), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  EXPECT_FALSE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
}

TEST(Manifest, ATableNamedAndMissingIsRefused) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    TableMeta meta = WriteTable(&t, state.next_file_number, {{IKey("a", 3), "1"}});
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = state.next_file_number + 1;
    ASSERT_TRUE(m->AppendGroup({Add(meta), bump}).ok());
    ASSERT_TRUE(m->Close().ok());
    ASSERT_TRUE(t.env()->DeleteFile(TablePath(kDir, meta.number)).ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  EXPECT_FALSE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
}

TEST(Manifest, ATableNumberAtOrAboveTheCounterIsRefused) {
  // Otherwise the next allocation collides with a live table, and a collision
  // is a file silently overwritten rather than an error.
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    const TableMeta meta = WriteTable(&t, 900, {{IKey("a", 3), "1"}});
    ASSERT_TRUE(m->AppendGroup({Add(meta)}).ok());  // counter left far below
    ASSERT_TRUE(m->Close().ok());
  }
  ManifestState state;
  std::unique_ptr<Manifest> m;
  const Status s = Manifest::Open(t.env(), kDir, &state, nullptr, &m);
  EXPECT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("900")) << s.ToString();
}

// ------------------------------------------- which kinds a manifest may hold

TEST(Manifest, AGroupTerminatorCarryingASequenceIsRefused) {
  // D7'S FORWARD BINDING, at the one field in this file shaped like a
  // watermark. The writer writes zero; the reader refuses anything else. BOTH
  // DIRECTIONS: the zero case is every other test in this file.
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  const std::string path = ManifestPath(kDir, 9);
  {
    WritableFilePtr f;
    ASSERT_TRUE(t.env()->NewWritableFile(path, &f).ok());
    wal::LogWriter w(f.get());
    std::string header;
    wal::EncodeFileHeader(9, &header);
    ASSERT_TRUE(w.AddRecord(Slice(header)).ok());
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = 10;
    std::string edit;
    EncodeEdit(bump, &edit);
    ASSERT_TRUE(w.AddRecord(Slice(edit)).ok());
    std::string group;
    wal::EncodeGroupEnd(7, 1, &group);  // a durable sequence, in a manifest
    ASSERT_TRUE(w.AddRecord(Slice(group)).ok());
    ASSERT_TRUE(f->Sync().ok());
    ASSERT_TRUE(f->Close().ok());
  }
  PutFile(&t, CurrentPath(kDir), Slice("MANIFEST-000009\n"));
  ManifestState state;
  std::unique_ptr<Manifest> m;
  const Status s = Manifest::Open(t.env(), kDir, &state, nullptr, &m);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("durable sequence")) << s.ToString();
}

TEST(Manifest, AWalBatchInAManifestIsRefused) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  const std::string path = ManifestPath(kDir, 9);
  {
    WritableFilePtr f;
    ASSERT_TRUE(t.env()->NewWritableFile(path, &f).ok());
    wal::LogWriter w(f.get());
    std::string header;
    wal::EncodeFileHeader(9, &header);
    ASSERT_TRUE(w.AddRecord(Slice(header)).ok());
    std::vector<wal::Op> ops;
    wal::Op op;
    const std::string k = "k";
    const std::string v = "v";
    op.kind = wal::OpKind::kSet;
    op.key = Slice(k);
    op.value = Slice(v);
    ops.push_back(op);
    std::string batch;
    wal::EncodeBatch(1, ops, &batch);
    ASSERT_TRUE(w.AddRecord(Slice(batch)).ok());
    std::string group;
    wal::EncodeGroupEnd(0, 1, &group);
    ASSERT_TRUE(w.AddRecord(Slice(group)).ok());
    ASSERT_TRUE(f->Sync().ok());
    ASSERT_TRUE(f->Close().ok());
  }
  PutFile(&t, CurrentPath(kDir), Slice("MANIFEST-000009\n"));
  ManifestState state;
  std::unique_ptr<Manifest> m;
  const Status s = Manifest::Open(t.env(), kDir, &state, nullptr, &m);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("WAL batch")) << s.ToString();
}

TEST(Manifest, AManifestEditInAWalIsRefused) {
  // The OTHER half, and it belongs beside its twin rather than in the WAL's own
  // file: the pair is one property. Neither log may be opened as the other, and
  // a rig that checks one direction has said nothing about the second.
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  const std::string path = kDir + "/000001.log";
  {
    WritableFilePtr f;
    ASSERT_TRUE(t.env()->NewWritableFile(path, &f).ok());
    wal::LogWriter w(f.get());
    std::string header;
    wal::EncodeFileHeader(1, &header);
    ASSERT_TRUE(w.AddRecord(Slice(header)).ok());
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = 3;
    std::string edit;
    EncodeEdit(bump, &edit);
    ASSERT_TRUE(w.AddRecord(Slice(edit)).ok());
    std::string group;
    wal::EncodeGroupEnd(0, 0, &group);
    ASSERT_TRUE(w.AddRecord(Slice(group)).ok());
    ASSERT_TRUE(f->Sync().ok());
    ASSERT_TRUE(f->Close().ok());
  }
  wal::RecoveryResult r;
  const Status s = wal::Recover(t.env(), kDir, wal::Caps(), wal::RecoverOptions(), &r);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("manifest edit")) << s.ToString();
}

// ------------------------------------------------- corruption and torn tails

// Writes `groups` manifest edits, each in its own sync group, into `path` and
// returns the bytes. `with_header` decides whether a FILE_HEADER leads.
std::string ManifestBytes(TestEnvironment* t, const std::string& path,
                          bool with_header, uint64_t first_number, int groups,
                          uint64_t* second_group_payload_at) {
  WritableFilePtr f;
  EXPECT_TRUE(t->env()->NewWritableFile(path, &f).ok());
  wal::LogWriter w(f.get());
  if (with_header) {
    std::string header;
    wal::EncodeFileHeader(1, &header);
    EXPECT_TRUE(w.AddRecord(Slice(header)).ok());
  }
  for (int i = 0; i < groups; ++i) {
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = first_number + static_cast<uint64_t>(i);
    std::string edit;
    EncodeEdit(bump, &edit);
    const uint64_t at = w.offset();
    EXPECT_TRUE(w.AddRecord(Slice(edit)).ok());
    std::string group;
    wal::EncodeGroupEnd(0, 1, &group);
    EXPECT_TRUE(w.AddRecord(Slice(group)).ok());
    // The SECOND group: a header and one committed group precede it.
    if (i == 1 && second_group_payload_at != nullptr) {
      *second_group_payload_at = at + kWalFragmentHeaderBytes + 2;
    }
  }
  EXPECT_TRUE(f->Sync().ok());
  EXPECT_TRUE(f->Close().ok());
  return t->ContentNow(path);
}

TEST(Manifest, InteriorCorruptionIsRefusedAndATornTailIsNot) {
  // THE GATE FOR THE RESYNC CHANGE, and it is a PAIR because section 5.4's rule
  // is a discriminator rather than a verdict: the SAME damaged byte is a torn
  // tail when nothing structurally valid follows and interior corruption when
  // something does. Only the pair says the discriminator works.
  //
  // Under the WAL's own predicate the second half is unreachable for a
  // manifest: edits carry no sequence and manifest terminators carry zero, so
  // `seq > committed` is false for everything a manifest contains, every
  // interior corruption classifies as a tail, and committed manifest state is
  // SILENTLY DISCARDED. BM51 is that predicate restored.
  //
  // Resync advances to the next BLOCK BOUNDARY -- the one alignment always
  // known to be a legal fragment start -- so the "something follows" image
  // places its records exactly there. The offsets come from the writer's own
  // reported position rather than from counting.
  TestEnvironment src;
  ASSERT_TRUE(src.env()->CreateDir(kDir).ok());
  uint64_t damage_at = 0;
  const std::string head = ManifestBytes(&src, kDir + "/head", true, 100, 3, &damage_at);
  const std::string more = ManifestBytes(&src, kDir + "/more", false, 200, 3, nullptr);
  ASSERT_GT(damage_at, 0u);
  ASSERT_LT(head.size(), kWalBlockBytes);
  ASSERT_LT(more.size(), kWalBlockBytes);

  std::string flipped = head;
  flipped[damage_at] = static_cast<char>(flipped[damage_at] ^ 0x01);

  // (a) DAMAGE WITH NOTHING AFTER IT: a torn tail. The committed prefix stands
  //     and discarding the rest is not an error.
  {
    TestEnvironment t;
    ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
    PutFile(&t, ManifestPath(kDir, 1), Slice(flipped));
    PutFile(&t, CurrentPath(kDir), Slice("MANIFEST-000001\n"));
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    EXPECT_EQ(100u, state.next_file_number - 1)
        << "the first committed group did not replay";
    ASSERT_TRUE(m->Close().ok());
  }

  // (b) THE SAME DAMAGE, with structurally valid records at the next block
  //     boundary: interior corruption, and the open is refused.
  {
    std::string image = flipped;
    image.append(kWalBlockBytes - image.size(), '\0');
    image.append(more);
    TestEnvironment t;
    ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
    PutFile(&t, ManifestPath(kDir, 1), Slice(image));
    PutFile(&t, CurrentPath(kDir), Slice("MANIFEST-000001\n"));
    ManifestState state;
    std::unique_ptr<Manifest> m;
    const Status s = Manifest::Open(t.env(), kDir, &state, nullptr, &m);
    ASSERT_FALSE(s.ok()) << "committed manifest state was silently discarded";
    EXPECT_NE(std::string::npos, s.ToString().find("interior corruption")) << s.ToString();
  }
}

TEST(Manifest, ATornTailIsDiscardedAndTheRestReplays) {
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  std::string good;
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = 42;
    ASSERT_TRUE(m->AppendGroup({bump}).ok());
    ASSERT_TRUE(m->Close().ok());
    good = t.ContentNow(ManifestPath(kDir, 1));
  }
  TestEnvironment torn;
  ASSERT_TRUE(torn.env()->CreateDir(kDir).ok());
  // Cut mid-record: the last group is incomplete, and nothing follows it.
  PutFile(&torn, ManifestPath(kDir, 1), Slice(good.data(), good.size() - 3));
  PutFile(&torn, CurrentPath(kDir), Slice("MANIFEST-000001\n"));
  ManifestState state;
  std::unique_ptr<Manifest> m;
  ASSERT_TRUE(Manifest::Open(torn.env(), kDir, &state, nullptr, &m).ok());
  // The snapshot group written by the FIRST open survived; the bump did not.
  EXPECT_EQ(2u, state.next_file_number - 1);
  ASSERT_TRUE(m->Close().ok());
}

TEST(Manifest, CurrentIsParsedStrictly) {
  // CURRENT is the one file in the tree whose entire job is to name another
  // one, so a lenient parse opens the wrong manifest because a byte changed.
  //
  // A REAL MANIFEST-000001 IS PUT IN THE DIRECTORY FIRST, and that is the whole
  // difference between this test and the one that let BM52 live. Without it
  // every malformed body below was refused because the manifest it named did
  // not exist -- a lenient parse failed too, for a reason that had nothing to
  // do with parsing, and the test never created the situation it was checking.
  // With manifest 1 present, "MANIFEST-1\n" and "MANIFEST-00000x\n" are bodies
  // a lenient parse RESOLVES, so refusing them has to come from the parse.
  TestEnvironment seed;
  ASSERT_TRUE(seed.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(seed.env(), kDir, &state, nullptr, &m).ok());
    ASSERT_EQ(1u, m->number());
    ASSERT_TRUE(m->Close().ok());
  }
  const std::string live = seed.ContentNow(ManifestPath(kDir, 1));
  ASSERT_FALSE(live.empty());

  // The control: this body IS accepted, so the refusals below are about the
  // bodies and not about the fixture.
  {
    TestEnvironment t;
    ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
    PutFile(&t, ManifestPath(kDir, 1), Slice(live));
    PutFile(&t, CurrentPath(kDir), Slice("MANIFEST-000001\n"));
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
    ASSERT_TRUE(m->Close().ok());
  }

  const char* bad[] = {"", "MANIFEST-1\n", "MANIFEST-000001", "manifest-000001\n",
                       "MANIFEST-00000x\n", "MANIFEST-000001\n\n",
                       "MANIFEST-000001x\n", "MANIFEST-0000001\n"};
  for (const char* body : bad) {
    TestEnvironment t;
    ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
    PutFile(&t, ManifestPath(kDir, 1), Slice(live));
    PutFile(&t, CurrentPath(kDir), Slice(body));
    ManifestState state;
    std::unique_ptr<Manifest> m;
    EXPECT_FALSE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok())
        << "accepted CURRENT body: " << body;
  }
}

// ---------------------------------------------------- the rename and its sync

TEST(Manifest, CurrentIsInstalledByRenameFollowedByADirectorySync) {
  // HARNESS-SIDE, from the ledger: the rig's own record of which Env calls were
  // issued, in order. It asks the engine nothing. A rename whose directory is
  // never synced is durable in the page cache and absent after a power cut --
  // section 3.3's injector exists for exactly this pair, and this is the first
  // call site in the tree that has one.
  TestEnvironment t;
  ASSERT_TRUE(t.env()->CreateDir(kDir).ok());
  ManifestState state;
  std::unique_ptr<Manifest> m;
  ASSERT_TRUE(Manifest::Open(t.env(), kDir, &state, nullptr, &m).ok());
  ASSERT_TRUE(m->Close().ok());

  bool renamed = false;
  bool synced_after = false;
  for (const auto& e : t.ledger()) {
    if (e.site == CallSite::kEnvRenameFile) { renamed = true; continue; }
    if (renamed && e.site == CallSite::kDirectorySync) { synced_after = true; break; }
  }
  EXPECT_TRUE(renamed) << "CURRENT was not installed by a rename";
  EXPECT_TRUE(synced_after) << "the rename's directory was never synced";
}

TEST(Manifest, AKillBeforeTheSwapLeavesTheOldManifestLive) {
  // The crash-consistency claim for this step: an Open that dies before the
  // rename leaves CURRENT naming the manifest it named before, and the new one
  // is an orphan the next Open removes.
  TestEnvironment probe;
  ASSERT_TRUE(probe.env()->CreateDir(kDir).ok());
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(probe.env(), kDir, &state, nullptr, &m).ok());
    ManifestEdit bump;
    bump.kind = EditKind::kNextFileNumber;
    bump.number = 55;
    ASSERT_TRUE(m->AppendGroup({bump}).ok());
    ASSERT_TRUE(m->Close().ok());
  }
  const testenv::DurableImage before = probe.Image();

  uint64_t rename_ordinal = 0;
  {
    auto t2 = TestEnvironment::FromImage(before, FaultPlan());
    ManifestState state;
    std::unique_ptr<Manifest> m;
    ASSERT_TRUE(Manifest::Open(t2->env(), kDir, &state, nullptr, &m).ok());
    ASSERT_TRUE(m->Close().ok());
    for (const auto& e : t2->ledger()) {
      if (e.site == CallSite::kEnvRenameFile) { rename_ordinal = e.ordinal; break; }
    }
  }
  ASSERT_GT(rename_ordinal, 0u);

  FaultPlan plan;
  plan.At(rename_ordinal, Injection::kKill);
  auto killed = TestEnvironment::FromImage(before, plan);
  {
    ManifestState state;
    std::unique_ptr<Manifest> m;
    (void)Manifest::Open(killed->env(), kDir, &state, nullptr, &m);
  }
  auto after = TestEnvironment::FromImage(killed->Image(), FaultPlan());
  ManifestState state;
  std::unique_ptr<Manifest> m;
  ASSERT_TRUE(Manifest::Open(after->env(), kDir, &state, nullptr, &m).ok());
  EXPECT_EQ(55u, state.next_file_number - 1);
  ASSERT_TRUE(m->Close().ok());
}

}  // namespace
}  // namespace sst
}  // namespace rift
