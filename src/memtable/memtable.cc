#include "memtable.h"

#include <cstring>

#include "check.h"

namespace rift {
namespace {

void PutFixed32(char* dst, uint32_t v) {
  dst[0] = static_cast<char>(v & 0xff);
  dst[1] = static_cast<char>((v >> 8) & 0xff);
  dst[2] = static_cast<char>((v >> 16) & 0xff);
  dst[3] = static_cast<char>((v >> 24) & 0xff);
}
uint32_t GetFixed32(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}
void PutFixed64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}
uint64_t GetFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

}  // namespace

// entry layout: [u32 internal_key_len][user_key][u64 tag][u32 value_len][value]
Slice MemTable::EntryUserKey(const char* entry) {
  const uint32_t ikey_len = GetFixed32(entry);
  RIFT_CHECK(ikey_len >= 8);
  return Slice(entry + 4, ikey_len - 8);
}
uint64_t MemTable::EntryTag(const char* entry) {
  const uint32_t ikey_len = GetFixed32(entry);
  return GetFixed64(entry + 4 + ikey_len - 8);
}
Slice MemTable::EntryValue(const char* entry) {
  const uint32_t ikey_len = GetFixed32(entry);
  const char* p = entry + 4 + ikey_len;
  const uint32_t vlen = GetFixed32(p);
  return Slice(p + 4, vlen);
}

// User key ascending by memcmp, then tag DESCENDING so the newest version of a
// key sorts first and a snapshot read is one seek.
int MemTable::CompareEntry(const char* entry, Slice user_key, uint64_t tag) {
  const int c = EntryUserKey(entry).compare(user_key);
  if (c != 0) return c;
  const uint64_t t = EntryTag(entry);
  if (t > tag) return -1;
  if (t < tag) return 1;
  return 0;
}

MemTable::Node* MemTable::NewNode(const char* entry, int height) {
  char* mem = arena_.Allocate(sizeof(Node));
  Node* n = reinterpret_cast<Node*>(mem);
  n->entry = entry;
  n->height = height;
  // The next-pointer array is a SEPARATE arena allocation rather than a
  // trailing `Node* next[1]` overrun. LevelDB uses the trailing-array trick;
  // here it would be a constant out-of-bounds index that UBSan's array-bounds
  // check reports, and a sanitizer lane that has to be argued with about our
  // own deliberate UB is a sanitizer lane that gets suppressions added to it.
  char* links = arena_.Allocate(sizeof(Node*) * static_cast<std::size_t>(height));
  n->next = reinterpret_cast<Node**>(links);
  for (int i = 0; i < height; ++i) n->next[i] = nullptr;
  return n;
}

MemTable::Node* MemTable::FindGreaterOrEqual(Slice user_key, uint64_t tag,
                                             Node** prev) const {
  Node* x = head_;
  int level = height_ - 1;
  while (true) {
    Node* next = x->next[level];
    if (next != nullptr && CompareEntry(next->entry, user_key, tag) < 0) {
      x = next;
      continue;
    }
    if (prev != nullptr) prev[level] = x;
    if (level == 0) return next;
    --level;
  }
}

MemTable::Node* MemTable::FindLessThan(Slice user_key, uint64_t tag) const {
  Node* x = head_;
  int level = height_ - 1;
  while (true) {
    Node* next = x->next[level];
    if (next != nullptr && CompareEntry(next->entry, user_key, tag) < 0) {
      x = next;
      continue;
    }
    if (level == 0) return (x == head_) ? nullptr : x;
    --level;
  }
}

MemTable::Node* MemTable::FindLast() const {
  Node* x = head_;
  int level = height_ - 1;
  while (true) {
    Node* next = x->next[level];
    if (next != nullptr) { x = next; continue; }
    if (level == 0) return (x == head_) ? nullptr : x;
    --level;
  }
}

void MemTable::Iter::SeekToFirst() {
  std::lock_guard<std::mutex> guard(table_->mu_);
  node_ = (table_->head_ == nullptr) ? nullptr : table_->head_->next[0];
}

void MemTable::Iter::SeekToLast() {
  std::lock_guard<std::mutex> guard(table_->mu_);
  node_ = (table_->head_ == nullptr) ? nullptr : table_->FindLast();
}

void MemTable::Iter::Seek(Slice user_key, uint64_t tag) {
  std::lock_guard<std::mutex> guard(table_->mu_);
  node_ = (table_->head_ == nullptr)
              ? nullptr
              : table_->FindGreaterOrEqual(user_key, tag, nullptr);
}

void MemTable::Iter::Next() {
  std::lock_guard<std::mutex> guard(table_->mu_);
  if (node_ != nullptr) node_ = node_->next[0];
}

void MemTable::Iter::Prev() {
  std::lock_guard<std::mutex> guard(table_->mu_);
  if (node_ == nullptr) return;
  node_ = table_->FindLessThan(EntryUserKey(node_->entry), EntryTag(node_->entry));
}

Slice MemTable::Iter::user_key() const { return EntryUserKey(node_->entry); }
uint64_t MemTable::Iter::tag() const { return EntryTag(node_->entry); }
Slice MemTable::Iter::value() const { return EntryValue(node_->entry); }

void MemTable::Add(SeqNum seq, ValueType type, Slice user_key, Slice value) {
  const uint64_t tag = MakeTag(seq, type);
  const uint32_t ikey_len = static_cast<uint32_t>(user_key.size() + 8);
  const std::size_t bytes = 4 + ikey_len + 4 + value.size();

  std::lock_guard<std::mutex> guard(mu_);

  if (head_ == nullptr) {
    // The head has no entry and full height; nothing ever compares against it.
    char* sentinel = arena_.Allocate(4 + 8);
    PutFixed32(sentinel, 8);
    PutFixed64(sentinel + 4, 0);
    head_ = NewNode(sentinel, kMaxHeight);
  }

  char* entry = arena_.Allocate(bytes);
  PutFixed32(entry, ikey_len);
  if (!user_key.empty()) std::memcpy(entry + 4, user_key.data(), user_key.size());
  PutFixed64(entry + 4 + user_key.size(), tag);
  char* vp = entry + 4 + ikey_len;
  PutFixed32(vp, static_cast<uint32_t>(value.size()));
  if (!value.empty()) std::memcpy(vp + 4, value.data(), value.size());

  Node* prev[kMaxHeight];
  for (int i = 0; i < kMaxHeight; ++i) prev[i] = head_;
  Node* found = FindGreaterOrEqual(user_key, tag, prev);

  // B1-D10: one sequence per batch, collapsed, so no two memtable entries ever
  // share a (user_key, seq) pair. Asserted rather than assumed -- the collapse
  // is what keeps this engine's sequence space aligned with engine/model's, and
  // a rig that needed a translation table between them would be a rig with a
  // place to be wrong.
  RIFT_CHECK(found == nullptr || CompareEntry(found->entry, user_key, tag) != 0);

  const int height = TowerHeight(user_key);
  if (height > height_) {
    for (int i = height_; i < height; ++i) prev[i] = head_;
    height_ = height;
  }

  Node* n = NewNode(entry, height);
  for (int i = 0; i < height; ++i) {
    n->next[i] = prev[i]->next[i];
    prev[i]->next[i] = n;
  }
  ++count_;
}

Status MemTable::Get(Slice user_key, SeqNum snapshot, std::string* value) const {
  std::lock_guard<std::mutex> guard(mu_);
  if (head_ == nullptr) return Status::NotFound(user_key.ToString());

  // Seek to the newest version at or below the snapshot: tags sort descending,
  // so the first entry not newer than the snapshot is the one we want.
  const uint64_t tag = MakeTag(snapshot, ValueType::kValue);
  Node* n = FindGreaterOrEqual(user_key, tag, nullptr);
  if (n == nullptr) return Status::NotFound(user_key.ToString());
  if (EntryUserKey(n->entry) != user_key) return Status::NotFound(user_key.ToString());

  const uint64_t found_tag = EntryTag(n->entry);
  // A RANGE TOMBSTONE ABOVE THE VERSION HIDES IT, exactly as a point deletion
  // would. STRICTLY above: within one batch every op shares a sequence, and a
  // Set issued after a DeleteRange must survive it -- which is the model's
  // intra-batch rule, and the reason this is `>` and not `>=`.
  if (NewestCoveringLocked(user_key, snapshot) > SeqOfTag(found_tag)) {
    return Status::NotFound(user_key.ToString());
  }
  const ValueType type = static_cast<ValueType>(found_tag & 0xff);
  if (type == ValueType::kDeletion) return Status::NotFound(user_key.ToString());
  *value = EntryValue(n->entry).ToString();
  return Status::Ok();
}

void MemTable::AddRangeTombstone(SeqNum seq, Slice start, Slice end,
                                 bool end_unbounded) {
  std::lock_guard<std::mutex> guard(mu_);
  MemRange r;
  r.start.assign(start.data(), start.size());
  if (!end_unbounded) r.end.assign(end.data(), end.size());
  r.end_unbounded = end_unbounded;
  r.seq = seq;
  ranges_.push_back(std::move(r));
}

SeqNum MemTable::NewestCovering(Slice user_key, SeqNum snapshot) const {
  std::lock_guard<std::mutex> guard(mu_);
  return NewestCoveringLocked(user_key, snapshot);
}

SeqNum MemTable::NewestCoveringLocked(Slice user_key, SeqNum snapshot) const {
  SeqNum best = 0;
  for (const MemRange& r : ranges_) {
    if (r.seq > snapshot) continue;
    if (r.seq <= best) continue;
    if (r.Covers(user_key)) best = r.seq;
  }
  return best;
}

std::vector<MemRange> MemTable::Ranges() const {
  std::lock_guard<std::mutex> guard(mu_);
  return ranges_;
}

std::size_t MemTable::RangeCount() const {
  std::lock_guard<std::mutex> guard(mu_);
  return ranges_.size();
}

std::size_t MemTable::MemoryUsage() const {
  std::lock_guard<std::mutex> guard(mu_);
  // THE TOMBSTONES COUNT. They do not live in the arena, and a memtable holding
  // nothing but range deletions would otherwise never reach the flush threshold
  // -- which is exactly the clear-everything workload `[A3]` named.
  std::size_t ranges = 0;
  for (const MemRange& r : ranges_) {
    ranges += sizeof(MemRange) + r.start.size() + r.end.size();
  }
  return arena_.MemoryUsage() + ranges;
}

std::size_t MemTable::Count() const {
  std::lock_guard<std::mutex> guard(mu_);
  return count_;
}

uint64_t MemTable::StructuralDigest() const {
  std::lock_guard<std::mutex> guard(mu_);
  uint64_t h = 0xcbf29ce484222325ULL;
  auto mix = [&h](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 0x100000001b3ULL;
    }
  };
  mix(static_cast<uint64_t>(height_));
  if (head_ != nullptr) {
    for (Node* n = head_->next[0]; n != nullptr; n = n->next[0]) {
      // The HEIGHT is what makes this a structural digest rather than a content
      // one. A height source that is not a pure function of the key changes
      // this number between two runs of the same workload; nothing else here
      // would notice.
      mix(static_cast<uint64_t>(n->height));
      const Slice k = EntryUserKey(n->entry);
      mix(k.size());
      for (std::size_t i = 0; i < k.size(); ++i) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(k.data()[i]));
        h *= 0x100000001b3ULL;
      }
      mix(EntryTag(n->entry));
    }
  }
  return h;
}

}  // namespace rift
