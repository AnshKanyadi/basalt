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

class VersionModel {
 public:
  void NoteWrite(const std::string& user_key, ModelSeq seq, bool deletion,
                 const std::string& value);

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
  std::set<ModelSeq> live_snapshots_;
  ModelSeq visible_ = 0;
};

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_VERSION_MODEL_H_
