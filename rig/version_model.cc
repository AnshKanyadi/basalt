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
    for (ModelSeq s : observable) {
      // The NEWEST version at or below s. Ascending order, so walk backwards
      // and stop at the first one that fits.
      for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if (it->seq > s) continue;
        // A DELETION IS NOT REQUIRED -- see the header. The answer at `s` is
        // kNotFound, and dropping the deletion preserves it provided nothing
        // older survives, which the resurrection rule checks instead.
        if (!it->deletion) out.insert({entry.first, it->seq});
        break;
      }
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
