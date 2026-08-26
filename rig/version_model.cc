#include "version_model.h"

#include <algorithm>

#include "check.h"

namespace rift {
namespace rig {

void VersionModel::NoteWrite(const std::string& user_key, ModelSeq seq,
                             bool deletion, const std::string& value) {
  std::vector<ModelVersion>& v = by_key_[user_key];
  // SEQUENCES ARE UNIQUE PER WRITE, so a duplicate is the harness contradicting
  // itself and not something to merge quietly.
  for (const ModelVersion& e : v) RIFT_CHECK(e.seq != seq);
  ModelVersion m;
  m.user_key = user_key;
  m.seq = seq;
  m.deletion = deletion;
  m.value = deletion ? std::string() : value;
  v.push_back(m);
  std::sort(v.begin(), v.end());
  if (seq > visible_) visible_ = seq;
}

void VersionModel::NoteDeleteRange(const std::string& start, const std::string& end,
                                  ModelSeq seq) {
  // The harness may not submit a range no writer could mean -- the same refusal
  // the classifier makes about the bytes, made here about the submission, so a
  // fixture cannot describe something the engine would refuse to write and then
  // hold the engine to it.
  RIFT_CHECK(start < end);
  ModelRange r;
  r.start = start;
  r.end = end;
  r.seq = seq;
  ranges_.push_back(r);
  if (seq > visible_) visible_ = seq;
}

std::vector<ModelRange> VersionModel::RangesCovering(const std::string& user_key) const {
  std::vector<ModelRange> out;
  for (const ModelRange& r : ranges_) {
    if (r.Covers(user_key)) out.push_back(r);
  }
  std::sort(out.begin(), out.end(),
            [](const ModelRange& a, const ModelRange& b) { return a.seq > b.seq; });
  return out;
}

void VersionModel::NoteSnapshotTaken(ModelSeq seq) { live_snapshots_.insert(seq); }
void VersionModel::NoteSnapshotReleased(ModelSeq seq) { live_snapshots_.erase(seq); }
void VersionModel::NoteVisibleSeq(ModelSeq seq) {
  RIFT_CHECK(seq >= visible_);  // the visible sequence never goes backwards
  visible_ = seq;
}

std::vector<ModelSeq> VersionModel::ObservableSequences() const {
  std::vector<ModelSeq> out(live_snapshots_.begin(), live_snapshots_.end());
  out.push_back(visible_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::set<VersionId> VersionModel::Required() const {
  const std::vector<ModelSeq> observable = ObservableSequences();
  std::set<VersionId> out;
  for (const auto& entry : by_key_) {
    const std::vector<ModelVersion>& versions = entry.second;  // ascending
    const std::vector<ModelRange> covering = RangesCovering(entry.first);
    for (ModelSeq s : observable) {
      // The NEWEST version at or below s. Ascending order, so walk backwards
      // and stop at the first one that fits.
      const ModelVersion* newest = nullptr;
      for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if (it->seq > s) continue;
        newest = &*it;
        break;
      }
      if (newest == nullptr) continue;  // nothing of this key is visible at s
      // AND THE NEWEST RANGE TOMBSTONE AT OR BELOW s, which competes with it.
      // `covering` is newest first, so the first one that fits is the winner.
      ModelSeq shadow = 0;
      bool shadowed = false;
      for (const ModelRange& r : covering) {
        if (r.seq > s) continue;
        shadow = r.seq;
        shadowed = true;
        break;
      }
      // WHICHEVER IS NEWER DECIDES THE ANSWER. A range tombstone above the
      // version hides it exactly as a point deletion would, so nothing is
      // required at this `s`.
      if (shadowed && shadow > newest->seq) continue;
      // A DELETION IS NOT REQUIRED -- see the header. The answer at `s` is
      // kNotFound, and dropping the deletion preserves it provided nothing
      // older survives, which the resurrection rule checks instead.
      if (!newest->deletion) out.insert({entry.first, newest->seq});
    }
  }
  return out;
}

std::set<VersionId> VersionModel::All() const {
  std::set<VersionId> out;
  for (const auto& entry : by_key_) {
    for (const ModelVersion& m : entry.second) out.insert({entry.first, m.seq});
  }
  return out;
}

std::vector<ModelVersion> VersionModel::VersionsOf(const std::string& user_key) const {
  const auto it = by_key_.find(user_key);
  if (it == by_key_.end()) return {};
  std::vector<ModelVersion> out(it->second.rbegin(), it->second.rend());
  return out;
}

bool VersionModel::IsDeletion(const VersionId& id) const {
  const auto it = by_key_.find(id.first);
  if (it == by_key_.end()) return false;
  for (const ModelVersion& m : it->second) {
    if (m.seq == id.second) return m.deletion;
  }
  return false;
}

}  // namespace rig
}  // namespace rift
