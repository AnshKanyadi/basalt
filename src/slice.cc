#include "basalt/slice.h"

#include <algorithm>

namespace basalt {

int Slice::compare(const Slice& b) const {
  const std::size_t min_len = std::min(size_, b.size_);
  int r = (min_len == 0) ? 0 : std::memcmp(data_, b.data_, min_len);
  if (r != 0) return r;
  if (size_ < b.size_) return -1;
  if (size_ > b.size_) return 1;
  return 0;
}

}  // namespace basalt
