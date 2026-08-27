#include "amplification.h"

#include <cstdio>
#include <memory>
#include <string>

#include "check.h"
#include "db.h"
#include "internal_key.h"
#include "manifest_image.h"
#include "table.h"
#include "test_env.h"

namespace rift {
namespace rig {
namespace {

const std::string kDir = "amp";

// A DETERMINISTIC KEY ORDER THAT IS NOT SORTED, which is what "fillrandom"
// means for amplification: keys arriving in key order would let every flush
// produce a table disjoint from the last, so no compaction would ever rewrite
// anything and the number measured would be a fact about the workload.
//
// It is `internal/rng`'s job on the Go side; here it is a fixed multiplicative
// step over a prime modulus -- deterministic, dependency-free, and reproducible
// from the seed printed with the result.
uint64_t Scramble(uint64_t i) { return (i * 2654435761u) % 1000000007u; }

std::string KeyOf(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "k%015llu",
                static_cast<unsigned long long>(Scramble(i)));
  return buf;
}

}  // namespace

uint64_t CrossingPointBytes(const wal::Caps& caps) {
  // 8 = the 10x write-amplification limit, less the WAL copy and the flush
  // copy. See the header and DESIGN-B3 section 8.1.
  return 8ull * 4ull * caps.flush_bytes;
}

AmpResult MeasureAmplification(const wal::Caps& caps,
                               const std::vector<uint64_t>& live_targets) {
  AmpResult r;
  r.caps = caps;
  r.crossing_bytes = CrossingPointBytes(caps);

  for (uint64_t target : live_targets) {
    testenv::TestEnvironment t;
    std::unique_ptr<DB> db;
    RIFT_CHECK(DB::Open(t.env(), kDir, caps, &db).ok());

    const std::string value(256, 'v');
    AmpPoint p;
    uint64_t i = 0;
    // DISTINCT KEYS, so `live_bytes` is what the database holds rather than
    // what was submitted: an overwrite-heavy workload would make space
    // amplification a statement about the overwrite rate.
    // CF-3: the progress quantity is `p.live_bytes`, which rises by
    // `k.size() + value.size()` -- A POSITIVE CONSTANT -- on every iteration.
    // It is independent of the engine entirely: the harness counts what it
    // SUBMITTED, so a bug in the engine cannot stall this loop, and a bug in
    // this loop cannot be masked by one in the engine.
    //
    // ITS CORRECTNESS INSTRUMENT IS `AmpInstrument.*`, which asserts the
    // numbers the loop produces are in the ranges only a working instrument can
    // produce. Termination says the measurement ends; it says nothing about
    // whether the number means anything.
    while (p.live_bytes < target) {
      const std::string k = KeyOf(i++);
      WriteBatch b;
      b.Set(Slice(k), Slice(value));
      wal::SeqNum s = 0;
      RIFT_CHECK(db->Write(b, &s).ok());
      p.submitted_bytes += k.size() + value.size();
      p.live_bytes += k.size() + value.size();
      if ((i % 64) == 0) {
        wal::SeqNum mark = 0;
        RIFT_CHECK(db->Sync(&mark).ok());
      }
    }
    wal::SeqNum mark = 0;
    RIFT_CHECK(db->Sync(&mark).ok());

    // WRITTEN BYTES COME FROM THE HARNESS'S LEDGER, not from the engine. Every
    // Append the engine made through Env is counted, so the WAL copy, every
    // flushed table and every compaction rewrite are all in it -- which is
    // exactly what write amplification is asking about.
    for (const testenv::LedgerEntry& e : t.ledger()) {
      p.written_bytes += e.append_bytes;
    }
    // A ZERO HERE WOULD BE THE INSTRUMENT, NOT THE ENGINE -- the first version
    // summed `durable_bytes_after`, which is a FILE SIZE after a Sync and is
    // left at zero for an Append, and reported 0.00. It announced itself only
    // because zero cannot be true.
    RIFT_CHECK(p.written_bytes > 0);

    // AND HOW MUCH OF L0 IS STILL UNPAID FOR. Parsed from the manifest, which
    // is an artifact -- never asked of the engine.
    {
      std::string current = t.ContentNow(sst::CurrentPath(kDir));
      while (!current.empty() && current.back() == '\n') current.pop_back();
      const std::string image = t.ContentNow(kDir + "/" + current);
      sst::ManifestState st;
      std::string why;
      if (ReplayManifestImage(Slice(image), &st, &why)) {
        for (const auto& e : st.tables) {
          if (e.second.level == 0) ++p.l0_at_end;
        }
      }
    }

    // DISK BYTES ARE COUNTED FROM THE DIRECTORY, by the harness, for B3-D8's
    // stated reason: asking the engine would be asking the thing under test.
    std::vector<std::string> children;
    RIFT_CHECK(t.env()->GetChildren(kDir, &children).ok());
    for (const std::string& c : children) {
      const std::string path = kDir + "/" + c;
      const std::string bytes = t.ContentNow(path);
      p.disk_bytes += bytes.size();
      if (c.size() > 4 && c.compare(c.size() - 4, 4, ".sst") == 0) ++p.tables;
    }

    // READ AMPLIFICATION, MEASURED FROM THE ARTIFACTS AND NOT FROM THE ENGINE.
    //
    // THE FROZEN INTERFACE GAINS NOTHING FOR THIS. Adding a counter to `DB`
    // would have been the easy way and would have put a measurement hook in the
    // interface both engines implement, for one phase's benefit.
    //
    // The harness opens the tables the directory holds -- which B3-D2a permits,
    // they are artifacts -- and counts, for each sampled key, the tables a
    // reader would have to consult: every table whose BLOOM FILTER says "maybe"
    // and whose key range admits it. A table the filter excludes is not
    // consulted, which is why this is MEASURED rather than derived from the
    // level structure: the structural answer is |L0| + 1 and takes no account
    // of the filter, whose entire purpose is to make the real number smaller.
    {
      std::vector<std::shared_ptr<sst::Table>> tables;
      for (const std::string& c : children) {
        if (c.size() <= 4 || c.compare(c.size() - 4, 4, ".sst") != 0) continue;
        std::shared_ptr<sst::Table> tbl;
        uint64_t number = 0;
        for (std::size_t j = 0; j + 4 < c.size(); ++j) {
          number = number * 10 + static_cast<uint64_t>(c[j] - '0');
        }
        if (sst::Table::Open(t.env(), kDir + "/" + c, number, &tbl).ok()) {
          tables.push_back(std::move(tbl));
        }
      }
      // A FIXED SAMPLE OF KEYS THAT ARE PRESENT. Absent keys would measure the
      // filter's false-positive rate, which is a different number.
      const uint64_t sample = i < 256 ? i : 256;
      uint64_t consulted = 0;
      for (uint64_t j = 0; j < sample; ++j) {
        const std::string k = KeyOf(j * (i / (sample == 0 ? 1 : sample)));
        for (const auto& tbl : tables) {
          if (!tbl->MayContain(Slice(k))) continue;
          const Slice lo = ExtractUserKey(Slice(tbl->check().smallest_key));
          const Slice hi = ExtractUserKey(Slice(tbl->check().largest_key));
          if (k < lo.ToString() || k > hi.ToString()) continue;
          ++consulted;
        }
      }
      p.read = sample == 0 ? 0.0
                           : static_cast<double>(consulted) /
                                 static_cast<double>(sample);
    }

    p.space = p.live_bytes == 0 ? 0.0
                                : static_cast<double>(p.disk_bytes) /
                                      static_cast<double>(p.live_bytes);
    p.write = p.submitted_bytes == 0
                  ? 0.0
                  : static_cast<double>(p.written_bytes) /
                        static_cast<double>(p.submitted_bytes);
    r.points.push_back(p);
    RIFT_CHECK(db->Close().ok());
  }

  for (const AmpPoint& p : r.points) {
    if (p.live_bytes >= r.crossing_bytes) r.above_crossing = true;
  }
  return r;
}

}  // namespace rig
}  // namespace rift
