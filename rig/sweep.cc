#include "sweep.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <utility>

#include "call_site.h"
#include "check.h"
#include "db.h"
#include "durable_mirror.h"
#include "test_env.h"

namespace rift {
namespace rig {
namespace {

using testenv::TestEnvironment;

const char* kDir = "db";

// THE FIXED WORKLOAD. Small on purpose: B1 has no flush, so the memtable and the
// WAL set grow without bound and every B1 test is sized accordingly. It is
// chosen to contain more than one group and more than one batch per group, since
// a sweep over a single-batch group cannot tell a group boundary from a batch
// boundary and would pass BM15 without noticing.
struct Driver {
  DB* db;
  SubmissionLog log;
  bool alive = true;
  // The Env ordinal at which the current Sync began, so the harness can ask
  // about the calls THIS Sync made rather than about every call in the run.
  const TestEnvironment* env = nullptr;
  uint64_t sync_start_ordinal = 0;

  void Put(const std::string& k, const std::string& v) {
    if (!alive) return;
    WriteBatch b;
    b.Set(Slice(k), Slice(v));
    wal::SeqNum s = 0;
    if (!db->Write(b, &s).ok()) { alive = false; return; }
    RefChange c;
    c.key = k;
    c.value = v;
    c.present = true;
    log.NoteWrite({c});
    log.NoteDurableSeq(static_cast<OracleSeq>(db->DurableSeq()));
  }
  void Sync() {
    if (!alive) return;
    if (env != nullptr) sync_start_ordinal = env->ordinal();
    log.NoteSyncStart();
    wal::SeqNum mark = 0;
    const Status s = db->Sync(&mark);
    if (s.code() == Status::Code::kKilled) { alive = false; return; }
    if (!s.ok()) { log.NoteSyncFailed(); return; }
    log.NoteSyncReturned(static_cast<OracleSeq>(mark));
    log.NoteDurableSeq(static_cast<OracleSeq>(db->DurableSeq()));
  }
};

// THE WORKLOAD, AND THE ONE THING B2 BROUGHT FORWARD (B2-Q3).
//
// The six-key prefix is B1's, unchanged, and it is what the default regime
// runs. The tail after it exists ONLY so the flush regime crosses its
// threshold: B2's gates on the flush path cannot be induced by a workload that
// never flushes, and that was the whole of the borrow list.
//
// It is guarded by the regime rather than always present, so the default
// regime's numbers stay comparable to the ones B1's floors were measured
// against. A longer workload in both regimes would have moved every floor for
// a reason unrelated to any defect.
void Workload(Driver* d, SweepRegime regime) {
  d->Put("a", "1");
  d->Put("b", "2");
  d->Sync();
  d->Put("c", "3");
  d->Put("d", "4");
  d->Put("e", "5");
  d->Sync();
  d->Put("f", "6");
  d->Sync();
  if (regime != SweepRegime::kFlush) return;
  // Enough to cross kFlushBytes at the flush regime's setting, so that the
  // Sync below runs a flush and every Env call it makes becomes a kill point.
  const std::string filler(256, 'x');
  for (int i = 0; i < 40; ++i) {
    char key[16];
    std::snprintf(key, sizeof key, "k%03d", i);
    d->Put(key, filler);
  }
  d->Sync();
  d->Put("after-the-flush", "1");
  d->Sync();
}

std::map<std::string, std::string> ExtractState(const DB& db) {
  std::map<std::string, std::string> out;
  std::unique_ptr<Iterator> it = db.NewIter(IterOptions());
  for (bool ok = it->First(); ok; ok = it->Next()) {
    out[it->Key().ToString()] = it->Value().ToString();
  }
  (void)it->Close();
  return out;
}

LedgerFacts FactsFrom(const TestEnvironment& t, bool in_flight,
                      uint64_t sync_start_ordinal) {
  LedgerFacts f;
  f.sync_in_flight = in_flight;
  // THE FACT IS "DID THIS Sync MAKE THE IN-FLIGHT GROUP DURABLE", and B2 broke
  // three assumptions the old one-line version rested on.
  //
  //   THE WAL, NOT THE LAST FILE. A group lives in the WAL. Until B2 the WAL
  //   was the only file this engine ever synced, so "the last Sync in the
  //   ledger" and "the WAL's Sync" were the same entry -- true by accident of
  //   there being one file. The flush syncs three.
  //
  //   ANY CALL THAT PROMOTED, NOT ONLY A Sync. A torn injection at a FLUSH
  //   promotes a prefix, and the promotion is recorded on the FLUSH entry. A
  //   filter that looked only at Sync calls missed it and reported "not
  //   durable" about bytes that were.
  //
  //   ANY PROMOTION IN THIS Sync, NOT THE LAST ONE. Durability is not undone.
  //   The flush creates a second WAL inside the same Sync and that empty file
  //   promotes nothing, so reading the LAST entry reports "not durable" about a
  //   group made durable moments earlier.
  //
  // All three were found by the kill-point sweep, where each blamed the engine
  // for the harness. Scoping to THIS Sync is what makes "any" safe: without it
  // one successful Sync answers for every group after it -- which is precisely
  // how the first two stayed invisible through all of B1.
  for (const testenv::LedgerEntry& e : t.ledger()) {
    if (e.ordinal <= sync_start_ordinal) continue;
    if (!e.promoted) continue;
    if (e.path.size() < 4 || e.path.compare(e.path.size() - 4, 4, ".log") != 0) continue;
    f.in_flight_durability_applied = true;
  }
  return f;
}

// SECTION 7.6, CLOSED HARNESS-SIDE. Every code the engine can return during the
// sweep, and the predicate that says when it was legitimate -- computed from
// what the harness knows, never from what the engine said.
bool PredicateSatisfied(Status::Code code, const TestEnvironment& t) {
  switch (code) {  // NO default: arm -- a new code must be classified
    case Status::Code::kOk:
      return true;
    case Status::Code::kKilled:
      return t.dead();  // the fault controller's dead flag
    case Status::Code::kIoError:
    case Status::Code::kDiskFull:
    case Status::Code::kCorruption:
      // Not injected by this sweep. Reaching one is a divergence: the engine
      // produced an error the harness cannot justify from its own record.
      return false;
    case Status::Code::kNotFound:
    case Status::Code::kRecordTooLarge:
    case Status::Code::kWalBufferFull:
    case Status::Code::kInvalidArgument:
      return false;
  }
  return false;
}

}  // namespace

const char* SweepRegimeName(SweepRegime r) {
  switch (r) {  // NO default: arm
    case SweepRegime::kDefault: return "default";
    case SweepRegime::kFlush:   return "flush";
  }
  RIFT_UNREACHABLE("SweepRegime holds a value no enumerator names");
}

wal::Caps CapsFor(SweepRegime r) {
  wal::Caps c;
  switch (r) {  // NO default: arm
    case SweepRegime::kDefault:
      return c;
    case SweepRegime::kFlush:
      // Low enough that the workload's tail crosses it, high enough that a
      // single batch does not: the regime is about flushing, not about the
      // batch that happened to trip the threshold.
      c.flush_bytes = 8u * 1024;
      return c;
  }
  RIFT_UNREACHABLE("SweepRegime holds a value no enumerator names");
}

uint64_t WorkloadOrdinalCount(SweepRegime regime) {
  TestEnvironment probe;
  std::unique_ptr<DB> db;
  if (!DB::Open(probe.env(), kDir, CapsFor(regime), &db).ok()) return 0;
  Driver d{db.get(), {}, true, &probe, 0};
  Workload(&d, regime);
  return probe.ordinal();
}

SweepResult RunSweep(SweepRegime regime) {
  SweepResult r;
  const wal::Caps caps = CapsFor(regime);
  const uint64_t n = WorkloadOrdinalCount(regime);

  // THREE MODES AT EVERY POINT, and the third was added because measuring the
  // sweep's power showed it was missing.
  //
  //   before the effect   -- the plain kill point
  //   after  the effect   -- the bytes are durable, the caller never learned it
  //   torn                -- a PREFIX of the newly covered extent survives
  //
  // The first two alone can never leave a BATCH on disk without its GROUP_END:
  // a kill before the effect promotes nothing and a kill after it promotes the
  // whole group. So a sweep with only those two modes cannot detect an engine
  // that commits uncommitted batches, which is the single thing the group
  // marker exists to prevent. Measured at 0 detections before the torn mode
  // existed; the floors file records what it is now.
  //
  // The prefixes are fixed and small, chosen to land inside and between the
  // batch records this workload writes. They are not a search: a deterministic
  // sweep with authored prefixes replays, and a randomized one does not.
  static const uint64_t kTornPrefixes[] = {8, 40, 80};
  const int kModes = 2 + static_cast<int>(sizeof(kTornPrefixes) / sizeof(uint64_t));

  for (uint64_t ordinal = 1; ordinal <= n; ++ordinal) {
    for (int after = 0; after < kModes; ++after) {
      SweepPoint p;
      p.ordinal = ordinal;
      p.after_effect = (after == 1);

      testenv::FaultPlan plan;
      if (after < 2) {
        plan.At(ordinal, p.after_effect ? testenv::Injection::kKillAfterEffect
                                        : testenv::Injection::kKill);
      } else {
        plan.At(ordinal, testenv::Injection::kTornSync, kTornPrefixes[after - 2]);
      }
      TestEnvironment t(plan);
      SubmissionLog log;
      uint64_t sync_start_ordinal = 0;
      bool opened = false;
      {
        std::unique_ptr<DB> db;
        const Status open = DB::Open(t.env(), kDir, caps, &db);
        if (open.ok()) {
          opened = true;
          Driver d{db.get(), {}, true, &t, 0};
          Workload(&d, regime);
          log = d.log;
          sync_start_ordinal = d.sync_start_ordinal;
        } else if (!PredicateSatisfied(open.code(), t)) {
          p.outcome = RunOutcome::kContractViolation;
          p.why = "Open returned " + std::string(CodeName(open.code())) +
                  " with no satisfied harness-side predicate";
        }
      }
      for (const testenv::LedgerEntry& e : t.ledger()) {
        if (e.ordinal == ordinal) p.call_site = CallSiteName(e.site);
      }

      if (!t.dead()) {
        // The point was never reached: the workload ended before this ordinal,
        // which happens for the after-effect pass of the very last call. It is
        // NOT counted, in the census or anywhere -- a census that counted
        // points nobody visited would report coverage that does not exist.
        continue;
      }
      r.points_visited++;
      if (!p.call_site.empty()) r.census[p.call_site]++;

      if (p.outcome == RunOutcome::kContractViolation) {
        r.violation++;
        r.failures.push_back(p);
        continue;
      }
      // A KILL DURING Open IS NOT INCONCLUSIVE, it is a run whose promised
      // state is empty. The engine must recover cleanly from a kill inside its
      // own Open -- and it is the one case where the durable image can hold a
      // half-created WAL, so calling it inconclusive would retire exactly the
      // points most worth checking. The submission log is empty, so the
      // expected state is empty, and the oracle compares that exactly like any
      // other watermark.
      (void)opened;

      std::unique_ptr<TestEnvironment> re =
          TestEnvironment::FromImage(t.Image(), testenv::FaultPlan());
      std::unique_ptr<DB> db;
      const Status reopen = DB::Open(re->env(), kDir, caps, &db);
      if (!reopen.ok()) {
        p.outcome = RunOutcome::kContractViolation;
        p.why = "reopen failed: " + reopen.ToString();
        r.violation++;
        r.failures.push_back(p);
        continue;
      }

      const RecoveryVerdict v =
          Adjudicate(log, FactsFrom(t, log.sync_in_flight(), sync_start_ordinal),
                     ExtractState(*db));
      // SECTION 7.5's SUPPRESSION, MECHANICAL. A run with a registry injector
      // enabled cannot be reported as anything but characterization, whatever
      // the verdict says.
      const RunOutcome floor = OutcomeFloor(t.exactness_suspended());
      p.outcome = (floor == RunOutcome::kCharacterizationOnly) ? floor : v.outcome;
      p.matched = v.matched;
      p.why = v.why;

      // THE CONTINUATION, and it closes a blind spot the sweep had.
      //
      // A reopened database keeps serving. Comparing only the state visible
      // immediately after recovery misses an engine that applied records it
      // never committed, because those land ABOVE the recovered watermark and
      // the snapshot hides them -- the data is in the memtable and simply
      // unreadable. That is not a defence, it is an accident of the read path:
      // at B2 the flush writes the memtable out and the uncommitted records
      // become durable, visible, and permanent.
      //
      // One write after reopening exposes it. The new write takes the sequence
      // the hidden records already occupy, so they become visible at exactly
      // the moment a real database would have resumed service.
      if (p.outcome == RunOutcome::kContractPass) {
        WriteBatch cont;
        cont.Set(Slice("zz"), Slice("9"));
        wal::SeqNum cs = 0;
        if (db->Write(cont, &cs).ok()) {
          std::map<std::string, std::string> expected = log.StateAt(v.seq);
          expected["zz"] = "9";
          // ENOUGH TO CROSS THE FLUSH THRESHOLD, in the regime that has one.
          //
          // A continuation of one small key never flushes: at a mid-workload
          // kill point the recovered memtable is far under the threshold, so
          // the Sync below writes no table and CF-1's mechanism is not exercised
          // at all. The filler is what makes "resuming service" include the
          // thing B2 added to service.
          //
          // It is regime-guarded because the default regime's threshold is four
          // megabytes: writing that at each of 300 kill points would make the
          // lane cost minutes to measure nothing, since by construction no flush
          // can occur there.
          if (regime == SweepRegime::kFlush) {
            const std::string filler(512, 'q');
            for (int i = 0; i < 40; ++i) {
              char key[24];
              std::snprintf(key, sizeof key, "zz-fill-%03d", i);
              const std::string k(key);
              WriteBatch fb;
              fb.Set(Slice(k), Slice(filler));
              wal::SeqNum fs = 0;
              if (!db->Write(fb, &fs).ok()) break;
              expected[k] = filler;
            }
          }
          wal::SeqNum cont_mark = 0;
          (void)db->Sync(&cont_mark);
          const std::map<std::string, std::string> after = ExtractState(*db);
          if (after != expected) {
            p.outcome = RunOutcome::kContractViolation;
            p.why = "after reopening and resuming service the state diverged: "
                    "recovery applied records it never committed, which the "
                    "recovered watermark was hiding";
          }

          // AND THEN A SECOND RECOVERY, WHICH IS CF-1's MECHANISM AND WAS NOT
          // BEING MEASURED.
          //
          // The continuation write above exposes hidden records ONE SEQUENCE AT
          // A TIME: it takes the sequence immediately above the watermark, so
          // anything higher stays under the snapshot. B1 accepted that because
          // it was all a WAL-only engine could show.
          //
          // The flush changes the mechanism entirely. Records recovery applied
          // but never committed are written out to a TABLE, and a table's
          // largest sequence is what the NEXT open takes its watermark from --
          // so on the second recovery they are not hidden at all, they are
          // promoted. That is CF-1's "durable, visible and permanent", and
          // observing it needs a second reopen rather than a second write.
          //
          // Measuring this is the whole of CF-1's obligation: without it the
          // sweep measures the continuation trick twice and reports that the
          // accidental defence has not expired, when what has not happened is
          // the measurement.
          if (p.outcome == RunOutcome::kContractPass) {
            const testenv::DurableImage second_image = re->Image();
            std::unique_ptr<TestEnvironment> re2 =
                TestEnvironment::FromImage(second_image, testenv::FaultPlan());
            std::unique_ptr<DB> db2;
            const Status reopen2 = DB::Open(re2->env(), kDir, caps, &db2);
            if (!reopen2.ok()) {
              p.outcome = RunOutcome::kContractViolation;
              p.why = "the second reopen failed: " + reopen2.ToString();
            } else if (ExtractState(*db2) != expected) {
              p.outcome = RunOutcome::kContractViolation;
              p.why = "after a flush and a second recovery the state diverged: "
                      "records recovery never committed were written to a table, "
                      "and the table's own sequence promoted them past the "
                      "watermark that was hiding them";
            }
          }
        }
      }

      switch (p.outcome) {  // NO default: arm
        case RunOutcome::kContractPass:         r.pass++; break;
        case RunOutcome::kContractViolation:    r.violation++; r.failures.push_back(p); break;
        case RunOutcome::kCharacterizationOnly: r.characterization++; break;
        case RunOutcome::kInconclusive:         r.inconclusive++; break;
        case RunOutcome::kVoid:                 r.voided++; break;
      }
      switch (v.matched) {  // NO default: arm
        case MatchedElement::kPreviousGroup: r.matched_previous++; break;
        case MatchedElement::kInFlightGroup: r.matched_in_flight++; break;
        case MatchedElement::kNone: break;
      }
    }
  }
  return r;
}

}  // namespace rig
}  // namespace rift
