// THE HARNESS'S OWN RECORD OF EVERY VERSION EVER SUBMITTED, and of which
// sequences a caller can still read at.
//
// NOT AN ORACLE. It produces no verdict; it is the RECORD a verdict is computed
// from, and it is built entirely from what the harness SUBMITTED. It never
// reads a byte the engine wrote.
//
// THAT IS B3-D2b AND IT IS THE LOAD-BEARING DECISION OF THE WHOLE PHASE:
//
//   `required` comes from the harness's submission log and NEVER from reading
//   the engine's files, because computing it from the engine's own artifacts
//   would hide a record from BOTH SIDES of the comparison and make the aliasing
//   TOTAL rather than one-sided.
//
//   Ruling 4 applied to a checker that is permitted to parse: PERMISSION TO
//   READ THE ARTIFACT IS NOT PERMISSION TO DERIVE THE EXPECTATION FROM IT.
//
// A reader who has internalised B3-D2a's two marks -- an artifact header
// declares nothing taking an Env* and nothing taking a snapshot -- will be
// tempted to source the expectation the same way. The marks say what an oracle
// may TOUCH. This says what it may CONCLUDE FROM. Only the first is mechanical,
// which is exactly why the second is written here in full.
#ifndef RIFT_RIG_VERSION_MODEL_H_
#define RIFT_RIG_VERSION_MODEL_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rift {
namespace rig {

using ModelSeq = uint64_t;

// One version, as the harness submitted it.
struct ModelVersion {
  std::string user_key;
  ModelSeq seq = 0;
  bool deletion = false;
  std::string value;  // empty for a deletion

  bool operator<(const ModelVersion& o) const {
    if (user_key != o.user_key) return user_key < o.user_key;
    return seq < o.seq;
  }
};

// A (user key, sequence) pair -- the identity of a version, which is what a
// drop is about.
using VersionId = std::pair<std::string, ModelSeq>;

// One range deletion, as the harness submitted it. `[start, end)`, half-open,
// agreeing with engine.InRange by construction.
//
// IT IS NOT A `ModelVersion` AND NOT A `VersionId`, deliberately. A version has
// one user key; a range tombstone has none of its own and shadows many. Folding
// it into the version space would mean inventing a key for it, and every set
// operation downstream would then be over a space with a fictional member in
// it.
struct ModelRange {
  std::string start;
  std::string end;
  ModelSeq seq = 0;

  // DELIBERATELY A SECOND IMPLEMENTATION OF `sst::RangeTombstone::Covers`, AND
  // IT MUST NOT BE REFACTORED INTO ONE.
  //
  // The obvious next change is to share the predicate, and it arrives wearing a
  // DRY argument: two copies of a half-open test is a boundary-key bug waiting
  // to happen. THAT ARGUMENT IS RIGHT ABOUT ORDINARY CODE AND WRONG HERE.
  //
  // B3-D2b: the harness may not derive its EXPECTATION from the engine. A
  // shared `Covers` is exactly that -- the checker would then agree with the
  // engine about which keys a range deletes BY CONSTRUCTION, and the one bug it
  // exists to catch, an off-by-one at the boundary, would be invisible from
  // both sides at once. That is GF-10's shape: a set of assertions all pointing
  // the same way has a blind spot the size of its agreement.
  //
  // The cost is real and is accepted: two half-open tests can drift. What keeps
  // them honest is that they are checked AGAINST EACH OTHER by every run, and a
  // drift shows up as a disagreement rather than as a shared silence.
  bool Covers(const std::string& user_key) const {
    return user_key >= start && user_key < end;
  }
};

class VersionModel {
 public:
  void NoteWrite(const std::string& user_key, ModelSeq seq, bool deletion,
                 const std::string& value);

  // A range deletion at `seq` covering `[start, end)`.
  void NoteDeleteRange(const std::string& start, const std::string& end,
                       ModelSeq seq);

  // Every submitted range tombstone covering `user_key`, NEWEST FIRST.
  std::vector<ModelRange> RangesCovering(const std::string& user_key) const;

  // Every range tombstone submitted, in submission order.
  const std::vector<ModelRange>& Ranges() const { return ranges_; }

  // Snapshots are recorded by SEQUENCE. A snapshot is live until released.
  void NoteSnapshotTaken(ModelSeq seq);
  void NoteSnapshotReleased(ModelSeq seq);

  // The newest sequence a caller can read at without a snapshot.
  void NoteVisibleSeq(ModelSeq seq);

  // `S`: the live-observable sequence set of B3-D1 -- every live snapshot, plus
  // the current visible sequence. A version no member of S can reach is
  // unobservable, and dropping the unobservable is the whole of what compaction
  // is permitted to do.
  std::vector<ModelSeq> ObservableSequences() const;

  // `keep(k)` for every k, unioned: for each s in S, the newest version of k
  // with seq <= s -- **IF IT IS A VALUE**.
  //
  // THE DELETION EXCLUSION IS A CORRECTION TO B3-D1 SECTION 1.2, FOUND BY
  // BUILDING THIS BEFORE ANY COMPACTION EXISTED. The claim as first written
  // required the newest version at every observable sequence to survive, full
  // stop. That OVER-REQUIRES: when the newest version is a DELETION, the answer
  // at that sequence is kNotFound, and dropping the deletion preserves the
  // answer exactly so long as nothing older survives to be uncovered.
  //
  // The requirement is on the ANSWER, not on a particular entry. So a deletion
  // is never in this set, and the obligation it does carry -- nothing older may
  // outlive it -- is checked separately, as the resurrection rule. A claim that
  // forbade dropping a tombstone with nothing left to mask would forbid the one
  // drop that makes compaction terminate in space.
  //
  // ---------------------------------------------------------------------
  // AND THE SAME CORRECTION EXTENDS OVER RANGES, WHICH IS THE POINT OF
  // WRITING THIS BEFORE THE WRITER EXISTS.
  //
  // A range tombstone at sequence `d` covering `[start, end)` shadows every
  // version of every key it covers with a sequence BELOW `d`. So at an
  // observable `s`, the answer for key `k` is decided by whichever is NEWER:
  // the newest point version of `k` at or below `s`, or the newest range
  // tombstone covering `k` at or below `s`.
  //
  //   newest thing is a VALUE      -> that value is required
  //   newest thing is a DELETION   -> the answer is kNotFound, nothing required
  //   newest thing is a RANGE TOMB -> the answer is kNotFound, nothing required
  //
  // A RANGE TOMBSTONE IS NEVER ITSELF REQUIRED, for the reason a point deletion
  // is not: the requirement is on the ANSWER at each observable sequence, never
  // on a particular entry surviving. What a range tombstone owes is the
  // resurrection rule -- nothing it masks may outlive it -- and that is checked
  // separately, over a RANGE rather than over a key.
  std::set<VersionId> Required() const;

  // Every version ever submitted. `Required()` is a subset.
  std::set<VersionId> All() const;

  // The versions of `user_key`, newest sequence first.
  std::vector<ModelVersion> VersionsOf(const std::string& user_key) const;

  // True if `id` is a deletion, as the harness submitted it.
  bool IsDeletion(const VersionId& id) const;

  std::size_t size() const { return by_key_.size(); }

 private:
  // user key -> versions, ascending by sequence. std::map so iteration order is
  // a property of the keys and never of insertion (section 6.1).
  std::map<std::string, std::vector<ModelVersion>> by_key_;
  // Submission order, never sorted: two range tombstones can share a sequence
  // (one batch, several DeleteRange ops) so there is no total order to impose,
  // and every query below is a filter rather than a lookup.
  std::vector<ModelRange> ranges_;
  std::set<ModelSeq> live_snapshots_;
  ModelSeq visible_ = 0;
};

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_VERSION_MODEL_H_
