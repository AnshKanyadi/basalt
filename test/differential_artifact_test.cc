// THE DIFFERENTIAL ARTIFACT'S CLASSIFIER, DRIVEN FROM HAND-BUILT BYTES.
//
// ---------------------------------------------------------------------------
// THE FIXTURES ARE BUILT FIELD BY FIELD FROM docs/FORMAT-differential.md, AND
// NEVER BY `EncodeDiffArtifact`. That is the condition on the two-decoder pair
// and it is the whole of what makes their independence real:
//
//   A FIXTURE BUILT BY THE PRODUCTION ENCODER TESTS THAT THE DECODER AGREES
//   WITH THE ENCODER -- WHICH IS THE QUESTION THIS PAIR EXISTS TO NOT ASSUME.
//
// It is the OPPOSITE of B2's rule for the SSTable classifier, where the fixture
// deliberately used `BlockBuilder` so that a second encoder could not be the
// thing under test. The difference is what is being checked: there, ONE decoder
// against a format; here, TWO decoders against EACH OTHER through a format. A
// shared encoder would be a shared blind spot across the language boundary.
//
// The same bytes are committed to `seeds/differential/format/` for the Go
// decoder, so the two sides share FIXTURES rather than CODE.
#include "differential_artifact.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crc32c.h"

namespace rift {
namespace rig {
namespace {

// ---------------------------------------------------------------- hand-built

void U8(std::string* o, uint8_t v) { o->push_back(static_cast<char>(v)); }
void U32(std::string* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void U64(std::string* o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void Str(std::string* o, const std::string& s) { U32(o, static_cast<uint32_t>(s.size())); o->append(s); }

std::string Provenance(const std::string& engine = "abc123",
                       const std::string& model = "def456",
                       const std::string& regime = "flush") {
  std::string p;
  Str(&p, engine);
  Str(&p, model);
  Str(&p, regime);
  U64(&p, 7);            // seed
  U64(&p, 4194304);      // flush_bytes
  U64(&p, 268435456);    // wal_buffer_bytes
  U64(&p, 33554432);     // max_record_bytes
  return p;
}

std::string OneSet(uint64_t seq = 1, const std::string& k = "a", const std::string& v = "1") {
  std::string p;
  U32(&p, 1);            // op_count
  U8(&p, 1);             // SET
  U64(&p, seq);
  Str(&p, k);
  Str(&p, v);
  U8(&p, 0);             // flags
  return p;
}

std::string Watermark(uint64_t w = 1) { std::string p; U64(&p, w); return p; }

std::string Recovered(const std::vector<std::pair<std::string, std::string>>& kvs) {
  std::string p;
  U32(&p, static_cast<uint32_t>(kvs.size()));
  for (const auto& kv : kvs) { Str(&p, kv.first); Str(&p, kv.second); }
  return p;
}

std::string Verdict(uint8_t outcome = 1, const std::string& why = "") {
  std::string p;
  U8(&p, outcome);
  Str(&p, why);
  return p;
}

struct Section { uint8_t kind; std::string payload; };

// Assembles an artifact from sections, computing the trailer. `count` defaults
// to the number of sections; a test can lie about it deliberately.
std::string Assemble(const std::vector<Section>& sections, int count = -1,
                     uint32_t version = kDiffFormatVersion) {
  std::string out;
  out.append(kDiffMagic, sizeof(kDiffMagic));
  U32(&out, version);
  U32(&out, count < 0 ? static_cast<uint32_t>(sections.size()) : static_cast<uint32_t>(count));
  for (const Section& s : sections) {
    U8(&out, s.kind);
    U32(&out, static_cast<uint32_t>(s.payload.size()));
    out.append(s.payload);
  }
  out.append(kDiffMagic, sizeof(kDiffMagic));
  U32(&out, wal::Crc32c(out.data(), out.size()));
  return out;
}

// The canonical LEGAL artifact. Every test below changes exactly one thing.
std::vector<Section> Good() {
  return {{1, Provenance()},
          {2, OneSet()},
          {3, Watermark()},
          {4, Recovered({{"a", "1"}})},
          {5, Verdict()}};
}

void Restamp(std::string* image) {
  const std::size_t crc_at = image->size() - 4;
  const uint32_t crc = wal::Crc32c(image->data(), crc_at);
  for (int i = 0; i < 4; ++i) {
    (*image)[crc_at + i] = static_cast<char>((crc >> (8 * i)) & 0xff);
  }
}

DiffCheck Check(const std::string& image, DiffArtifact* a) {
  return ParseDiffArtifact(Slice(image), a);
}

// ------------------------------------------------------------- the legal shape

TEST(DiffArtifact, AcceptsTheCanonicalArtifactAndReportsWhatItHolds) {
  DiffArtifact a;
  const std::string image = Assemble(Good());
  const DiffCheck v = Check(image, &a);
  ASSERT_TRUE(v.ok()) << DiffFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ("abc123", a.provenance.engine_commit);
  EXPECT_EQ("def456", a.provenance.model_commit);
  EXPECT_EQ("flush", a.provenance.regime);
  EXPECT_EQ(7u, a.provenance.seed);
  ASSERT_EQ(1u, a.submission.size());
  EXPECT_EQ(DiffOpKind::kSet, a.submission[0].kind);
  EXPECT_EQ("a", a.submission[0].key);
  EXPECT_EQ(1u, a.watermark);
  ASSERT_EQ(1u, a.recovered.size());
  EXPECT_EQ("1", a.recovered.at("a"));
  EXPECT_EQ(DiffOutcome::kAgree, a.outcome);
}

// THE BOUNDED FLAGS SURVIVE, which is the distinction db.h's divergence 3 is
// about: an empty key is a valid key, so emptiness cannot carry boundedness.
TEST(DiffArtifact, AnEmptyBoundIsDistinguishedFromAnUnboundedOne) {
  std::string ops;
  U32(&ops, 2);
  U8(&ops, 3); U64(&ops, 1); Str(&ops, ""); Str(&ops, ""); U8(&ops, 3);  // both bounded, both empty
  U8(&ops, 3); U64(&ops, 2); Str(&ops, ""); Str(&ops, ""); U8(&ops, 0);  // both unbounded
  std::vector<Section> s = Good();
  s[1].payload = ops;
  DiffArtifact a;
  const std::string image = Assemble(s);
  ASSERT_TRUE(Check(image, &a).ok());
  ASSERT_EQ(2u, a.submission.size());
  EXPECT_TRUE(a.submission[0].start_bounded);
  EXPECT_TRUE(a.submission[0].end_bounded);
  EXPECT_FALSE(a.submission[1].start_bounded);
  EXPECT_FALSE(a.submission[1].end_bounded);
}

// ------------------------------------------------------------- what it refuses

TEST(DiffArtifact, RefusesAForeignFile) {
  std::string image = Assemble(Good());
  image[0] = 'X';
  Restamp(&image);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kBadMagic, v.fault);
}

// AN INCOMPLETE FILE IS A DIFFERENT REPORT FROM A DAMAGED ONE, which is why the
// magic appears twice. Truncate and the header still parses.
TEST(DiffArtifact, RefusesATruncatedFileAsIncompleteRatherThanMalformed) {
  const std::string full = Assemble(Good());
  const std::string image = full.substr(0, full.size() - 20);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kBadTrailingMagic, v.fault) << v.why;
}

TEST(DiffArtifact, RefusesABadChecksum) {
  std::string image = Assemble(Good());
  image[image.size() - 5] ^= 0x01;   // inside the footer magic's neighbour
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_TRUE(v.fault == DiffFault::kBadChecksum ||
              v.fault == DiffFault::kBadTrailingMagic) << DiffFaultName(v.fault);
}

TEST(DiffArtifact, RefusesAFormatVersionItCannotPlace) {
  const std::string image = Assemble(Good(), -1, 2);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kUnknownFormatVersion, v.fault);
}

// REFUSED, NOT SKIPPED. An artifact whose unknown section is ignored has lost
// the thing it was meant to carry and reports success.
TEST(DiffArtifact, RefusesAnUnknownSectionKindRatherThanSkippingIt) {
  std::vector<Section> s = Good();
  s.push_back({9, "payload"});
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kUnknownSectionKind, v.fault);
}

TEST(DiffArtifact, RefusesADuplicateSection) {
  std::vector<Section> s = Good();
  s.push_back({5, Verdict()});
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  // A duplicate is also out of ascending order; either refusal is correct and
  // the test says so rather than pinning the one that happens to fire first.
  EXPECT_TRUE(v.fault == DiffFault::kDuplicateSection ||
              v.fault == DiffFault::kSectionsNotAscending) << DiffFaultName(v.fault);
}

TEST(DiffArtifact, RefusesSectionsOutOfAscendingOrder) {
  std::vector<Section> s = {{1, Provenance()},
                            {3, Watermark()},
                            {2, OneSet()},
                            {4, Recovered({{"a", "1"}})},
                            {5, Verdict()}};
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kSectionsNotAscending, v.fault);
}

TEST(DiffArtifact, RefusesAMissingSection) {
  std::vector<Section> s = Good();
  s.pop_back();  // no VERDICT
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kMissingSection, v.fault);
}

TEST(DiffArtifact, RefusesASectionRunningPastTheFooter) {
  std::string image = Assemble(Good());
  // Inflate the first section's declared length.
  const std::size_t len_at = 8 + 4 + 4 + 1;
  for (int i = 0; i < 4; ++i) image[len_at + i] = static_cast<char>(0xff);
  Restamp(&image);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kSectionRunsPastTheEnd, v.fault);
}

TEST(DiffArtifact, RefusesAnArtifactOfNoOperations) {
  std::vector<Section> s = Good();
  std::string empty;
  U32(&empty, 0);
  s[1].payload = empty;
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kEmptySubmission, v.fault);
}

TEST(DiffArtifact, RefusesRecoveredKeysThatDoNotAscend) {
  std::vector<Section> s = Good();
  s[3].payload = Recovered({{"b", "2"}, {"a", "1"}});
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kRecoveredNotAscending, v.fault);
}

TEST(DiffArtifact, RefusesDuplicateRecoveredKeys) {
  std::vector<Section> s = Good();
  s[3].payload = Recovered({{"a", "1"}, {"a", "2"}});
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok()) << "a duplicate key lets one decoder see what the other does not";
  EXPECT_EQ(DiffFault::kRecoveredNotAscending, v.fault);
}

// THE REGIME IS REQUIRED, AND IT IS NOT DERIVABLE FROM THE CAPS BESIDE IT: the
// `flush` and `compact` regimes share caps and differ in workload. Provenance
// that is derivable-in-principle is provenance nobody derives.
TEST(DiffArtifact, RefusesAnArtifactThatNamesNoRegime) {
  std::vector<Section> s = Good();
  s[0].payload = Provenance("abc123", "def456", "");
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kMissingRegime, v.fault);
}

// AN UNJUDGED ARTIFACT PARSES -- the driver cannot reach a verdict, because
// reaching one requires the model, which is Go. The corpus gate is separate.
TEST(DiffArtifact, AnUnjudgedArtifactParsesAndFailsTheCorpusGate) {
  std::vector<Section> s = Good();
  s[4].payload = Verdict(0);
  DiffArtifact a;
  const std::string image = Assemble(s);
  const DiffCheck parsed = Check(image, &a);
  ASSERT_TRUE(parsed.ok()) << DiffFaultName(parsed.fault) << ": " << parsed.why;
  EXPECT_EQ(DiffOutcome::kUnrun, a.outcome);

  const DiffCheck gate = RequireJudged(a);
  EXPECT_FALSE(gate.ok()) << "an unjudged artifact must not enter the corpus";
  EXPECT_EQ(DiffFault::kUnjudged, gate.fault);
}

// GF-14: THE OTHER HALF. A judged artifact passes the gate, or the gate could
// be one that refuses everything.
TEST(DiffArtifact, AJudgedArtifactPassesTheCorpusGate) {
  DiffArtifact a;
  const std::string image = Assemble(Good());
  ASSERT_TRUE(Check(image, &a).ok());
  EXPECT_TRUE(RequireJudged(a).ok());
}

TEST(DiffArtifact, RefusesAVerdictThatNamesNoEnumerator) {
  for (uint8_t bad : {uint8_t{5}, uint8_t{200}}) {
    std::vector<Section> s = Good();
    s[4].payload = Verdict(bad);
    const std::string image = Assemble(s);
    DiffArtifact a;
    const DiffCheck v = Check(image, &a);
    ASSERT_FALSE(v.ok()) << "outcome byte " << static_cast<int>(bad);
    EXPECT_EQ(DiffFault::kUnknownOutcome, v.fault);
  }
}

// B4-Q1: AN ARTIFACT NAMING NO COMMIT CANNOT BE REPRODUCED, which is the same
// defect as an exit run at an uncommitted tree naming a commit that does not
// contain what ran.
TEST(DiffArtifact, RefusesAnArtifactThatNamesNoCommit) {
  for (int which = 0; which < 2; ++which) {
    std::vector<Section> s = Good();
    s[0].payload = which == 0 ? Provenance("", "def456") : Provenance("abc123", "");
    const std::string image = Assemble(s);
    DiffArtifact a;
    const DiffCheck v = Check(image, &a);
    ASSERT_FALSE(v.ok()) << "commit " << which;
    EXPECT_EQ(DiffFault::kMissingCommit, v.fault);
  }
}

TEST(DiffArtifact, RefusesAnArtifactProducedAtADirtyTree) {
  for (int which = 0; which < 2; ++which) {
    std::vector<Section> s = Good();
    s[0].payload = which == 0 ? Provenance("abc123-dirty", "def456")
                              : Provenance("abc123", "def456-dirty");
    const std::string image = Assemble(s);
    DiffArtifact a;
    const DiffCheck v = Check(image, &a);
    ASSERT_FALSE(v.ok()) << "commit " << which;
    EXPECT_EQ(DiffFault::kDirtyCommit, v.fault);
  }
}

TEST(DiffArtifact, RefusesASubmissionWhoseSequencesGoBackwards) {
  std::string ops;
  U32(&ops, 2);
  U8(&ops, 1); U64(&ops, 5); Str(&ops, "a"); Str(&ops, "1"); U8(&ops, 0);
  U8(&ops, 1); U64(&ops, 3); Str(&ops, "b"); Str(&ops, "2"); U8(&ops, 0);
  std::vector<Section> s = Good();
  s[1].payload = ops;
  const std::string image = Assemble(s);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kSequencesNotMonotone, v.fault);
}

// GF-14: THE OTHER HALF. A SYNC consumes no sequence and carries 0, and a rule
// phrased as "strictly increasing" would refuse every legal artifact.
TEST(DiffArtifact, AcceptsAZeroSequenceForAnOperationThatConsumesNone) {
  std::string ops;
  U32(&ops, 3);
  U8(&ops, 1); U64(&ops, 5); Str(&ops, "a"); Str(&ops, "1"); U8(&ops, 0);
  U8(&ops, 4); U64(&ops, 0); Str(&ops, "");  Str(&ops, "");  U8(&ops, 0);  // SYNC
  U8(&ops, 1); U64(&ops, 6); Str(&ops, "b"); Str(&ops, "2"); U8(&ops, 0);
  std::vector<Section> s = Good();
  s[1].payload = ops;
  DiffArtifact a;
  const std::string image = Assemble(s);
  const DiffCheck v = Check(image, &a);
  ASSERT_TRUE(v.ok()) << DiffFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(3u, a.submission.size());
}

TEST(DiffArtifact, RefusesBytesAfterTheLastSection) {
  // Declare four sections but append five: the fifth's bytes then sit between
  // the last section the count admits and the footer.
  const std::string image = Assemble(Good(), 4);
  DiffArtifact a;
  const DiffCheck v = Check(image, &a);
  ASSERT_FALSE(v.ok());
  EXPECT_EQ(DiffFault::kTrailingBytes, v.fault);
}

// ------------------------------------- the encoder agrees with the classifier

// THIS IS THE ONLY TEST THAT USES THE ENCODER, and it is the round trip rather
// than a fixture: what it asserts is that the WRITER emits what the classifier
// accepts. Every refusal above is driven by hand-built bytes precisely so this
// one test cannot be the whole story.
TEST(DiffArtifact, WhatTheEncoderWritesIsWhatTheClassifierAccepts) {
  DiffArtifact in;
  in.provenance.engine_commit = "cafe";
  in.provenance.model_commit = "babe";
  in.provenance.regime = DiffRegimeName(DiffRegime::kCompact);
  in.provenance.seed = 99;
  in.provenance.flush_bytes = 4194304;
  DiffOp op;
  op.kind = DiffOpKind::kDeleteRange;
  op.seq = 3;
  op.key = "";
  op.value = "";
  op.start_bounded = false;
  op.end_bounded = false;
  in.submission.push_back(op);
  in.watermark = 3;
  in.recovered["k"] = "v";
  in.outcome = DiffOutcome::kAgree;

  const std::string image = EncodeDiffArtifact(in);
  DiffArtifact out;
  const DiffCheck v = Check(image, &out);
  ASSERT_TRUE(v.ok()) << DiffFaultName(v.fault) << ": " << v.why;
  EXPECT_EQ(in.provenance.engine_commit, out.provenance.engine_commit);
  EXPECT_EQ(in.watermark, out.watermark);
  ASSERT_EQ(1u, out.submission.size());
  EXPECT_EQ(DiffOpKind::kDeleteRange, out.submission[0].kind);
  EXPECT_FALSE(out.submission[0].start_bounded);
  EXPECT_EQ(in.recovered, out.recovered);
  EXPECT_EQ(DiffOutcome::kAgree, out.outcome);
}

// AND TWO ARTIFACTS OF ONE RUN ARE BYTE-IDENTICAL, which is what the ascending
// order rule buys and what makes the corpus diffable.
TEST(DiffArtifact, TheSameContentEncodesToTheSameBytes) {
  DiffArtifact in;
  in.provenance.engine_commit = "cafe";
  in.provenance.model_commit = "babe";
  in.provenance.regime = DiffRegimeName(DiffRegime::kFlush);
  DiffOp op;
  op.kind = DiffOpKind::kSet;
  op.seq = 1;
  op.key = "a";
  op.value = "1";
  in.submission.push_back(op);
  in.watermark = 1;
  in.recovered["b"] = "2";
  in.recovered["a"] = "1";   // inserted out of order on purpose
  in.outcome = DiffOutcome::kAgree;
  EXPECT_EQ(EncodeDiffArtifact(in), EncodeDiffArtifact(in));
}

// ------------------------------------------- THE SHARED FIXTURE CORPUS
//
// THE SAME BYTES THE GO DECODER READS, produced by NEITHER decoder. This is
// what makes the two implementations independent in fact rather than in
// intention: they are kept in step by a directory of files, not by shared code.
//
// A fixture generated by either encoder would test that that decoder agrees
// with that encoder -- the question the pair exists to not assume.

std::string ReadFixture(const std::string& name) {
  // The test binary runs from the build directory; the corpus is at the repo
  // root. A missing corpus FAILS rather than skipping: a pair whose shared
  // fixtures are absent is two decoders nobody has compared.
  const char* candidates[] = {
      "../../../seeds/differential/format/",
      "../../seeds/differential/format/",
      "seeds/differential/format/",
  };
  for (const char* dir : candidates) {
    std::ifstream f(std::string(dir) + name, std::ios::binary);
    if (!f) continue;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
  return std::string();
}

TEST(DiffFixtures, TheLegalOnesAreAccepted) {
  for (const char* name : {"legal-minimal.diff", "legal-every-op.diff",
                           "legal-unjudged.diff"}) {
    const std::string image = ReadFixture(name);
    ASSERT_FALSE(image.empty()) << "fixture " << name << " not found -- a pair "
                                   "whose shared fixtures are absent is two "
                                   "decoders nobody has compared";
    DiffArtifact a;
    const DiffCheck v = ParseDiffArtifact(Slice(image), &a);
    EXPECT_TRUE(v.ok()) << name << ": " << DiffFaultName(v.fault) << " " << v.why;
    EXPECT_FALSE(a.provenance.regime.empty()) << name;
  }
}

TEST(DiffFixtures, TheRefusedOnesAreRefused) {
  for (const char* name : {"refuse-bad-magic.diff",
                           "refuse-truncated.diff",
                           "refuse-bad-checksum.diff",
                           "refuse-version-2.diff",
                           "refuse-unknown-section.diff",
                           "refuse-duplicate-section.diff",
                           "refuse-sections-out-of-order.diff",
                           "refuse-missing-section.diff",
                           "refuse-empty-submission.diff",
                           "refuse-recovered-unsorted.diff",
                           "refuse-recovered-duplicate.diff",
                           "refuse-unknown-outcome.diff",
                           "refuse-no-engine-commit.diff",
                           "refuse-no-model-commit.diff",
                           "refuse-dirty-commit.diff",
                           "refuse-no-regime.diff",
                           "refuse-sequences-backwards.diff",
                           "refuse-trailing-bytes.diff"}) {
    const std::string image = ReadFixture(name);
    ASSERT_FALSE(image.empty()) << "fixture " << name << " not found";
    DiffArtifact a;
    const DiffCheck v = ParseDiffArtifact(Slice(image), &a);
    // THE TWO DECODERS NEED NOT AGREE ON *WHICH* REFUSAL FIRES when a file
    // breaks more than one rule -- only that neither accepts it.
    EXPECT_FALSE(v.ok()) << name << " was accepted";
  }
}

TEST(DiffFixtures, TheUnjudgedFixtureFailsTheCorpusGate) {
  const std::string image = ReadFixture("legal-unjudged.diff");
  ASSERT_FALSE(image.empty());
  DiffArtifact a;
  ASSERT_TRUE(ParseDiffArtifact(Slice(image), &a).ok());
  EXPECT_EQ(DiffOutcome::kUnrun, a.outcome);
  EXPECT_FALSE(RequireJudged(a).ok());
}

}  // namespace
}  // namespace rig
}  // namespace rift
