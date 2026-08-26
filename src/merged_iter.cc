#include "merged_iter.h"

#include "check.h"
#include "internal_key.h"

namespace rift {

bool MergedIter::Source::Valid() const {
  return mem != nullptr ? mem->Valid() : table->Valid();
}
void MergedIter::Source::Next() {
  if (mem != nullptr) mem->Next(); else table->Next();
}
void MergedIter::Source::Prev() {
  if (mem != nullptr) mem->Prev(); else table->Prev();
}
void MergedIter::Source::SeekToFirst() {
  if (mem != nullptr) mem->SeekToFirst(); else table->SeekToFirst();
}
void MergedIter::Source::SeekToLast() {
  if (mem != nullptr) mem->SeekToLast(); else table->SeekToLast();
}
void MergedIter::Source::Seek(Slice user_key, uint64_t tag,
                              const std::string& internal) {
  if (mem != nullptr) mem->Seek(user_key, tag); else table->Seek(Slice(internal));
}
Slice MergedIter::Source::user_key() const {
  return mem != nullptr ? mem->user_key() : ExtractUserKey(table->key());
}
uint64_t MergedIter::Source::tag() const {
  return mem != nullptr ? mem->tag() : ExtractTag(table->key());
}
Slice MergedIter::Source::value() const {
  return mem != nullptr ? mem->value() : table->value();
}

void MergedIter::AddMemTable(const MemTable* m) {
  Source s;
  s.mem.reset(new MemTable::Iter(m));
  sources_.push_back(std::move(s));
}

void MergedIter::AddTable(const sst::Table* t) {
  Source s;
  s.table.reset(new sst::Table::Iter(t));
  sources_.push_back(std::move(s));
}

void MergedIter::AddRun(std::vector<const sst::Table*> run) {
  if (run.empty()) return;
  Source s;
  s.table.reset(new sst::ConcatIter(std::move(run)));
  sources_.push_back(std::move(s));
}

int MergedIter::CompareSources(std::size_t i, std::size_t j) const {
  const int c = sources_[i].user_key().compare(sources_[j].user_key());
  if (c != 0) return c;
  // Tags DESCENDING inside a user key: the newest version sorts first. The same
  // order internal_key.h defines, applied to two live cursors instead of two
  // encoded keys.
  const uint64_t a = sources_[i].tag();
  const uint64_t b = sources_[j].tag();
  if (a > b) return -1;
  if (a < b) return 1;
  return 0;
}

void MergedIter::PickSmallest() {
  current_ = -1;
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    if (!sources_[i].Valid()) continue;
    if (current_ < 0 || CompareSources(i, static_cast<std::size_t>(current_)) < 0) {
      current_ = static_cast<int>(i);
    }
  }
}

void MergedIter::PickLargest() {
  current_ = -1;
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    if (!sources_[i].Valid()) continue;
    if (current_ < 0 || CompareSources(i, static_cast<std::size_t>(current_)) > 0) {
      current_ = static_cast<int>(i);
    }
  }
}

void MergedIter::SeekAllTo(Slice user_key, uint64_t tag) {
  seek_key_.clear();
  AppendInternalKey(&seek_key_, user_key, tag);
  for (Source& s : sources_) s.Seek(user_key, tag, seek_key_);
}

void MergedIter::SeekToFirst() {
  for (Source& s : sources_) s.SeekToFirst();
  dir_ = Direction::kForward;
  PickSmallest();
}

void MergedIter::SeekToLast() {
  for (Source& s : sources_) s.SeekToLast();
  dir_ = Direction::kReverse;
  PickLargest();
}

void MergedIter::Seek(Slice user_key, uint64_t tag) {
  SeekAllTo(user_key, tag);
  dir_ = Direction::kForward;
  PickSmallest();
}

void MergedIter::Next() {
  RIFT_CHECK(Valid());
  if (dir_ != Direction::kForward) {
    // Every source other than the current one is positioned BEFORE the current
    // key, because that is what reverse traversal left them at. Re-seek them
    // past it before asking which is smallest.
    const std::string key = user_key().ToString();
    const uint64_t t = tag();
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      if (static_cast<int>(i) == current_) continue;
      seek_key_.clear();
      AppendInternalKey(&seek_key_, Slice(key), t);
      sources_[i].Seek(Slice(key), t, seek_key_);
    }
    dir_ = Direction::kForward;
  }
  sources_[static_cast<std::size_t>(current_)].Next();
  PickSmallest();
}

void MergedIter::Prev() {
  RIFT_CHECK(Valid());
  if (dir_ != Direction::kReverse) {
    const std::string key = user_key().ToString();
    const uint64_t t = tag();
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      if (static_cast<int>(i) == current_) continue;
      seek_key_.clear();
      AppendInternalKey(&seek_key_, Slice(key), t);
      sources_[i].Seek(Slice(key), t, seek_key_);
      // Seek lands at the first entry >= the target; one step back is the last
      // entry strictly before it, which is where reverse traversal needs it.
      if (sources_[i].Valid()) sources_[i].Prev(); else sources_[i].SeekToLast();
    }
    dir_ = Direction::kReverse;
  }
  sources_[static_cast<std::size_t>(current_)].Prev();
  PickLargest();
}

Slice MergedIter::user_key() const {
  RIFT_CHECK(Valid());
  return sources_[static_cast<std::size_t>(current_)].user_key();
}
uint64_t MergedIter::tag() const {
  RIFT_CHECK(Valid());
  return sources_[static_cast<std::size_t>(current_)].tag();
}
Slice MergedIter::value() const {
  RIFT_CHECK(Valid());
  return sources_[static_cast<std::size_t>(current_)].value();
}

}  // namespace rift
