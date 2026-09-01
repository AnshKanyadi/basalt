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

  // A string LITERAL, whose storage is static and therefore outlives anything.
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}  // NOLINT

  // BINDING A SLICE TO A TEMPORARY STRING IS A COMPILE ERROR.
  //
  // A Slice is a pointer and a length owned by somebody else, so
  // `Slice(std::string(...))` produces one that dangles at the end of the full
  // expression -- and it reads exactly like the safe forms beside it. Without
  // this deletion, `Slice("k")` also went through the const std::string&
  // constructor and dangled, which is a bug this cycle actually had: it was
  // caught by ASan in the mutant lane's BASELINE GATE, in a test whose only
  // symptom was that it happened to keep the Slice past the expression.
  //
  // Deleting the overload converts the whole class into a build failure. Hold
  // the string in a named local and pass that.
  Slice(std::string&&) = delete;

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
