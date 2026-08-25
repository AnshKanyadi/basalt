#include "table.h"

#include <algorithm>

#include "check.h"
#include "read_whole_file.h"

namespace rift {
namespace sst {

Status Table::Open(Env* env, const std::string& path, uint64_t number,
                   std::shared_ptr<Table>* out) {
  std::shared_ptr<Table> t(new Table());
  Status s = ReadWholeFile(env, path, &t->image_);
  if (!s.ok()) return s;

  // VALIDATED BEFORE IT IS USABLE, by the classifier whose every rule was
  // induced against hand-built bytes in B2.0. A damaged table is a refused open
  // rather than a wrong answer, and the refusal names the byte.
  t->check_ = ValidateTable(Slice(t->image_));
  if (!t->check_.ok()) {
    return Status::Corruption(path + ": " + TableFaultName(t->check_.fault) +
                              " at offset " + std::to_string(t->check_.offset) +
                              " (" + t->check_.why + ")");
  }
  t->number_ = number;

  Footer footer;
  std::string why;
  RIFT_CHECK(DecodeFooter(Slice(t->image_), &footer, &why));

  std::vector<BlockEntry> index_entries;
  std::vector<uint32_t> restarts;
  const Slice index(t->image_.data() + footer.index.offset, footer.index.size);
  RIFT_CHECK(ParseBlock(index, &index_entries, &restarts, &why));
  for (const BlockEntry& e : index_entries) {
    BlockRef ref;
    RIFT_CHECK(DecodeHandle(e.value, &ref.handle));
    ref.last_key = e.key;
    t->blocks_.push_back(ref);
  }

  if (footer.filter.size != 0) {
    const Slice filter(t->image_.data() + footer.filter.offset, footer.filter.size);
    if (!FilterReader::Parse(filter, &t->filter_, &why)) {
      return Status::Corruption(path + ": filter block: " + why);
    }
  }
  *out = std::move(t);
  return Status::Ok();
}

Status Table::Get(Slice user_key, SeqNum snapshot, std::string* value,
                  bool* deleted, bool* filtered) const {
  *deleted = false;
  *filtered = false;
  if (!MayContain(user_key)) {
    *filtered = true;
    return Status::NotFound("");
  }
  std::string target;
  AppendInternalKey(&target, user_key, MakeTag(snapshot, ValueType::kValue));
  Iter it(this);
  it.Seek(Slice(target));
  if (!it.Valid()) return Status::NotFound("");
  const Slice found = it.key();
  if (ExtractUserKey(found) != user_key) return Status::NotFound("");
  // The first entry at or after (user_key, snapshot) in the internal order IS
  // the newest visible version: tags sort descending inside a user key, which
  // is the whole reason the internal key packs them that way.
  if (TypeOfTag(ExtractTag(found)) == ValueType::kDeletion) {
    *deleted = true;
    return Status::NotFound("");
  }
  *value = it.value().ToString();
  return Status::Ok();
}

void Table::Iter::LoadBlock(std::size_t i) {
  const BlockRef& ref = t_->blocks_[i];
  const Slice block(t_->image_.data() + ref.handle.offset, ref.handle.size);
  std::vector<uint32_t> restarts;
  std::string why;
  // ValidateTable already parsed every block successfully at Open, so a failure
  // here is not a damaged file -- it is this process disagreeing with itself.
  RIFT_CHECK(ParseBlock(block, &entries_, &restarts, &why));
  block_ = i;
  entry_ = 0;
  loaded_ = true;
}

void Table::Iter::SeekToFirst() {
  if (t_->blocks_.empty()) { block_ = 0; loaded_ = false; return; }
  LoadBlock(0);
}

void Table::Iter::SeekToLast() {
  if (t_->blocks_.empty()) { block_ = 0; loaded_ = false; return; }
  LoadBlock(t_->blocks_.size() - 1);
  entry_ = entries_.size() - 1;  // the classifier refuses an empty data block
}

void Table::Iter::Prev() {
  RIFT_CHECK(Valid());
  if (entry_ > 0) { --entry_; return; }
  if (block_ == 0) { block_ = t_->blocks_.size(); loaded_ = false; return; }
  LoadBlock(block_ - 1);
  entry_ = entries_.size() - 1;
}

void Table::Iter::Seek(Slice target) {
  // Binary search over the index: the first block whose LAST key is >= target.
  // The separator is that last key EXACTLY (B2-D2), so this comparison is
  // against a key the table really contains rather than against a synthesized
  // one -- which is also why the classifier can check it.
  std::size_t lo = 0;
  std::size_t hi = t_->blocks_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (CompareInternalKey(t_->blocks_[mid].last_key, target) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo >= t_->blocks_.size()) { block_ = t_->blocks_.size(); loaded_ = false; return; }
  LoadBlock(lo);
  while (entry_ < entries_.size() &&
         CompareInternalKey(entries_[entry_].key, target) < 0) {
    ++entry_;
  }
  if (entry_ >= entries_.size()) {
    // The target sorts after every entry of the block the index chose, which
    // can only happen at the very end of the table.
    block_ = t_->blocks_.size();
    loaded_ = false;
  }
}

void Table::Iter::Next() {
  RIFT_CHECK(Valid());
  ++entry_;
  while (entry_ >= entries_.size()) {
    if (block_ + 1 >= t_->blocks_.size()) {
      block_ = t_->blocks_.size();
      loaded_ = false;
      return;
    }
    LoadBlock(block_ + 1);
  }
}

Slice Table::Iter::key() const {
  RIFT_CHECK(Valid() && loaded_);
  return entries_[entry_].key;
}

Slice Table::Iter::value() const {
  RIFT_CHECK(Valid() && loaded_);
  return entries_[entry_].value;
}

}  // namespace sst
}  // namespace rift
