// BASALT_ORACLE -- registered in ORACLES.txt. It may PARSE artifacts and may not
// CONSULT beliefs; its src/ includes are allow-listed in ARTIFACTS.txt.
// THE EXACTNESS ORACLE. Section 7.3, and it asks the engine nothing.
//
// Ruling 4: "the crash rig's verdicts come from its own op log, never from
// asking the engine what it believes it holds." The recorded sentence is AN
// ORACLE THAT INTERROGATES THE ENGINE BELIEVES THE LIE.
//
// This header is the mechanism, not a promise about one. It includes NOTHING
// from engine-cpp/src -- no DB, no MemTable, no Wal, no Env, not even Status --
// so there is no engine state it could consult even by accident. Its inputs are
// three, and all three are the harness's own:
//
//   * the SUBMISSION LOG: every Write and every Sync the rig issued, in order,
//     with the sequences the rig assigned itself;
//   * LEDGER FACTS: what TestEnv recorded about whether durability was applied,
//     summarized harness-side;
//   * the RECOVERED STATE, already extracted into a plain map by an adapter
//     that is not this file.
//
// The first sentence was a lane in the parent project: an include of anything
// from src/ in this file or its .cc failed it. Here it is a rule held by review
// rather than by a scanner. Condition 1 of section 7.4 says
// the oracle must be "compiled against a header that does not include the
// engine's internal state at all", and that is a statement a lane can check.
//
// ---------------------------------------------------------------------------
// THE TWO-ELEMENT SET, AND WHY IT IS A SET.
//
//   R = G_k             when Sync k was applied fully
//   R in {G_{k-1}, G_k} when Sync k was in flight or torn at the kill
//
// A Sync can complete on the device with the kill preempting its return: the
// bytes are durable, the caller never learned it. No design removes that -- it
// is "did the RPC commit?", one layer down -- and ruling 3's "any watermark the
// sync-completion schedule can produce" is what covers it.
//
// EACH ELEMENT IS COMPARED EXACTLY AND THE VERDICT NAMES WHICH ONE MATCHED. Not
// a boolean: a verdict that cannot say which element it matched is a failure of
// the oracle, not a pass of the engine. And both elements are individually
// induced by tests, because A TWO-ELEMENT SET WHERE ONLY ONE ELEMENT HAS EVER
// BEEN OBSERVED IS A ONE-ELEMENT CONTRACT WITH A SPARE EXCUSE ATTACHED.
#ifndef BASALT_RIG_EXACTNESS_ORACLE_H_
#define BASALT_RIG_EXACTNESS_ORACLE_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "run_outcome.h"

namespace basalt {
namespace rig {

using OracleSeq = uint64_t;

// One effect the harness computed for itself. `present` false is a deletion.
struct RefChange {
  std::string key;
  std::string value;
  bool present = true;
};

// The harness's own record of what it issued. Nothing here is reported by the
// engine; the sequences are the rig's own count, and the engine's returned
// sequence is compared against them rather than trusted (see kSeqDivergence).
class SubmissionLog {
 public:
  // +1 per Write INCLUDING EMPTY ONES, which is engine/model's rule and
  // B1-D10's. A rig whose sequence space differed from the engine's would need
  // a translation table, and a rig that needs a translation table is a rig with
  // a place to be wrong.
  OracleSeq NoteWrite(const std::vector<RefChange>& changes);

  void NoteSyncStart();
  // Records that a Sync RETURNED, and the watermark it returned. This is the
  // only engine-reported number the oracle ever touches, and it appears only in
  // assertion (ii), as THE PROMISE BEING HELD TO -- never as the answer being
  // checked.
  void NoteSyncReturned(OracleSeq watermark);

  // A Sync that RETURNED UNSUCCESSFULLY. It is not in flight -- the caller got
  // an answer -- but no group closed, so the previous group's high sequence
  // stands.
  void NoteSyncFailed();

  // EVERY WATERMARK THE ENGINE EVER RETURNED IS A PROMISE, not only the ones a
  // Sync handed back. DurableSeq() is a public durability claim, and an engine
  // that advanced it early over-promises to any reader that asks -- which a
  // killed Sync's missing return value would otherwise hide completely, because
  // a process that died never returned anything at all.
  void NoteDurableSeq(OracleSeq reported);

  OracleSeq last_assigned() const { return next_ - 1; }
  OracleSeq highest_returned_watermark() const { return highest_returned_; }

  // The high sequence of the group that closed at the last RETURNED Sync.
  OracleSeq previous_group_high() const { return previous_group_high_; }
  // The high sequence of the group that was open when the log ended.
  OracleSeq in_flight_group_high() const { return last_assigned(); }
  bool sync_in_flight() const { return sync_in_flight_; }

  // The state the harness believes existed at sequence r, from its own record.
  std::map<std::string, std::string> StateAt(OracleSeq r) const;

  // Every sequence at which a Write landed, in order. Used only to prove the
  // oracle REFUSES intermediate boundaries (BM15).
  const std::vector<OracleSeq>& write_sequences() const { return write_seqs_; }

 private:
  struct Entry {
    OracleSeq seq = 0;
    std::vector<RefChange> changes;
  };
  std::vector<Entry> entries_;
  std::vector<OracleSeq> write_seqs_;
  OracleSeq next_ = 1;
  OracleSeq highest_returned_ = 0;
  OracleSeq previous_group_high_ = 0;
  bool sync_in_flight_ = false;
};

// What TestEnv's ledger says, summarized harness-side. Two booleans, because
// two is all the two-element set needs.
struct LedgerFacts {
  // A Sync was issued and never returned before the kill.
  bool sync_in_flight = false;
  // The ledger records that its durability WAS applied. This is the case that
  // makes R the in-flight group: the bytes landed, the caller never learned it.
  bool in_flight_durability_applied = false;
};

enum class MatchedElement : uint8_t {
  kNone,           // nothing matched -- a violation, never a pass
  kPreviousGroup,  // G_{k-1}
  kInFlightGroup,  // G_k
};
const char* MatchedElementName(MatchedElement m);

struct RecoveryVerdict {
  RunOutcome outcome = RunOutcome::kContractViolation;

  // WHICH ELEMENT. Not a boolean. A verdict that cannot say which element it
  // matched is a failure of the oracle, not a pass of the engine.
  MatchedElement matched = MatchedElement::kNone;
  OracleSeq seq = 0;
  std::size_t compared = 0;  // keys compared, so a vacuous pass is visible

  std::string why;  // on failure, in the operator's words
};

// (i) exactness and (ii) no over-promise, in one pass.
RecoveryVerdict Adjudicate(const SubmissionLog& log, const LedgerFacts& facts,
                           const std::map<std::string, std::string>& recovered);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_EXACTNESS_ORACLE_H_
