#include "exactness_oracle.h"

#include <utility>

namespace rift {
namespace rig {

const char* MatchedElementName(MatchedElement m) {
  switch (m) {  // NO default: arm
    case MatchedElement::kNone:          return "none";
    case MatchedElement::kPreviousGroup: return "G(k-1)";
    case MatchedElement::kInFlightGroup: return "G(k)";
  }
  return "invalid";
}

OracleSeq SubmissionLog::NoteWrite(const std::vector<RefChange>& changes) {
  Entry e;
  e.seq = next_++;
  e.changes = changes;
  write_seqs_.push_back(e.seq);
  entries_.push_back(std::move(e));
  return entries_.back().seq;
}

void SubmissionLog::NoteSyncStart() { sync_in_flight_ = true; }

void SubmissionLog::NoteSyncReturned(OracleSeq watermark) {
  sync_in_flight_ = false;
  if (watermark > highest_returned_) highest_returned_ = watermark;
  // The group that just closed. Computed from the rig's OWN count of what it
  // submitted, not from the returned watermark -- the watermark is held to,
  // never believed.
  previous_group_high_ = last_assigned();
}

void SubmissionLog::NoteSyncFailed() { sync_in_flight_ = false; }

void SubmissionLog::NoteDurableSeq(OracleSeq reported) {
  if (reported > highest_returned_) highest_returned_ = reported;
}

std::map<std::string, std::string> SubmissionLog::StateAt(OracleSeq r) const {
  std::map<std::string, std::string> state;
  for (const Entry& e : entries_) {
    if (e.seq > r) break;
    for (const RefChange& c : e.changes) {
      if (c.present) {
        state[c.key] = c.value;
      } else {
        state.erase(c.key);
      }
    }
  }
  return state;
}

namespace {

bool SameState(const std::map<std::string, std::string>& a,
               const std::map<std::string, std::string>& b, std::string* why) {
  if (a.size() != b.size()) {
    *why = "key count " + std::to_string(b.size()) + " != expected " +
           std::to_string(a.size());
    return false;
  }
  auto ai = a.begin();
  auto bi = b.begin();
  for (; ai != a.end(); ++ai, ++bi) {
    if (ai->first != bi->first) {
      *why = "key mismatch: expected '" + ai->first + "', found '" + bi->first + "'";
      return false;
    }
    if (ai->second != bi->second) {
      *why = "value mismatch at '" + ai->first + "'";
      return false;
    }
  }
  return true;
}

}  // namespace

RecoveryVerdict Adjudicate(const SubmissionLog& log, const LedgerFacts& facts,
                           const std::map<std::string, std::string>& recovered) {
  RecoveryVerdict v;
  v.compared = recovered.size();

  // Two harness-side records must agree about whether a Sync was in flight: the
  // rig's own call log, and TestEnv's ledger. This is not the forbidden kind of
  // agreement -- neither is the engine -- and a disagreement means the HARNESS
  // is broken, which is worth failing loudly rather than adjudicating through.
  if (facts.sync_in_flight != log.sync_in_flight()) {
    v.outcome = RunOutcome::kInconclusive;
    v.why = "harness disagrees with itself about whether a Sync was in flight";
    return v;
  }

  // THE SET, DERIVED FROM THE HARNESS'S OWN RECORD AND THE LEDGER. Never from
  // the engine, and never from a manifest -- B1-D7 removes the manifest as a
  // possible source by not having one.
  //
  // It is exactly one or exactly two elements. Anything wider is BM15: an
  // oracle that accepts any batch boundary inside the in-flight group has
  // stopped checking the thing GROUP_END exists to make checkable, which is
  // that a group commits whole or not at all.
  std::vector<std::pair<MatchedElement, OracleSeq>> candidates;
  if (!facts.sync_in_flight) {
    candidates.push_back({MatchedElement::kInFlightGroup, log.previous_group_high()});
  } else {
    candidates.push_back({MatchedElement::kPreviousGroup, log.previous_group_high()});
    if (facts.in_flight_durability_applied) {
      candidates.push_back({MatchedElement::kInFlightGroup, log.in_flight_group_high()});
    }
  }

  // (ii) NO OVER-PROMISE, CHECKED STRUCTURALLY AND BEFORE ANY COMPARISON.
  //
  // If the engine returned a watermark higher than ANY point the ledger can
  // justify, it over-promised -- and that is true whatever the recovered state
  // turns out to be. Checking it only when a candidate matched would make the
  // assertion the whole rig exists for conditional on the other assertion
  // passing first.
  OracleSeq highest_candidate = 0;
  for (const auto& c : candidates) {
    if (c.second > highest_candidate) highest_candidate = c.second;
  }
  if (log.highest_returned_watermark() > highest_candidate) {
    v.outcome = RunOutcome::kContractViolation;
    v.why = "over-promise: the engine returned watermark " +
            std::to_string(log.highest_returned_watermark()) +
            " but the ledger justifies at most " +
            std::to_string(highest_candidate);
    return v;
  }

  // (i) EXACTNESS. Each element compared exactly.
  for (const auto& c : candidates) {
    std::string why;
    if (SameState(log.StateAt(c.second), recovered, &why)) {
      v.matched = c.first;
      v.seq = c.second;
      v.outcome = RunOutcome::kContractPass;
      return v;
    }
    v.why = why;
  }

  // Neither element matched. DIAGNOSIS ONLY -- nothing below can produce a
  // pass. Naming the boundary the state actually landed on is the difference
  // between "the engine is wrong" and a bug report, and it is what tells
  // over-promise apart from landing inside a group.
  v.outcome = RunOutcome::kContractViolation;
  v.matched = MatchedElement::kNone;
  OracleSeq landed = 0;
  bool found = false;
  std::vector<OracleSeq> probes;
  probes.push_back(0);
  for (OracleSeq s : log.write_sequences()) probes.push_back(s);
  for (OracleSeq p : probes) {
    std::string ignored;
    if (SameState(log.StateAt(p), recovered, &ignored)) {
      landed = p;
      found = true;
    }
  }
  if (found && landed < log.highest_returned_watermark()) {
    v.why = "over-promise: the engine returned watermark " +
            std::to_string(log.highest_returned_watermark()) +
            " and recovery landed on " + std::to_string(landed) +
            " -- acknowledged data did not survive";
  } else if (found) {
    v.why = "recovery landed on sequence " + std::to_string(landed) +
            ", a batch boundary strictly inside a group: a group must commit "
            "whole or not at all";
  } else {
    v.why = "recovered state matches no sequence the harness ever submitted (" +
            v.why + ")";
  }
  return v;
}

}  // namespace rig
}  // namespace rift
