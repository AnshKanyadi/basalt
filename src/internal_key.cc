#include "internal_key.h"

#include "check.h"

namespace rift {

uint64_t MakeTag(SeqNum seq, ValueType type) {
  RIFT_CHECK(seq <= kMaxSeqNum);
  return (seq << 8) | static_cast<uint64_t>(type);
}

void AppendInternalKey(std::string* out, Slice user_key, uint64_t tag) {
  out->append(user_key.data(), user_key.size());
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((tag >> (8 * i)) & 0xff));
  }
}

Slice ExtractUserKey(Slice internal_key) {
  RIFT_CHECK(internal_key.size() >= kTagBytes);
  return Slice(internal_key.data(), internal_key.size() - kTagBytes);
}

uint64_t ExtractTag(Slice internal_key) {
  RIFT_CHECK(internal_key.size() >= kTagBytes);
  const char* p = internal_key.data() + internal_key.size() - kTagBytes;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

int CompareInternalKey(Slice a, Slice b) {
  const int c = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (c != 0) return c;
  const uint64_t ta = ExtractTag(a);
  const uint64_t tb = ExtractTag(b);
  // DESCENDING. The newest version of a key sorts first; see the header for
  // what a bytewise comparison does here instead, and why it looks fine.
  if (ta > tb) return -1;
  if (ta < tb) return 1;
  return 0;
}

}  // namespace rift
