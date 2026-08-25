// Slice: a pointer and a length, owned by somebody else.
#ifndef RIFT_SLICE_H_
#define RIFT_SLICE_H_

#include <cstddef>
#include <cstring>
#include <string>

namespace rift {

class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* d, std::size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}  // NOLINT

  const char* data() const { return data_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  std::string ToString() const { return std::string(data_, size_); }

  // Bytewise, and not pluggable. A pluggable comparator is the door through
  // which the storage engine learns what a key MEANS, and A5 puts MVCC
  // timestamps inside keys -- ruling 2 says the engine never interprets time,
  // and a fixed bytewise comparison makes that uncompilable rather than
  // remembered. DESIGN-B1 section 6.2.
  int compare(const Slice& b) const;

 private:
  const char* data_;
  std::size_t size_;
};

inline bool operator==(const Slice& a, const Slice& b) {
  return a.size() == b.size() &&
         (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
inline bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }

}  // namespace rift

#endif  // RIFT_SLICE_H_
