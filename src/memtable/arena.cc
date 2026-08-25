#include "arena.h"

#include "check.h"

namespace rift {

Arena::~Arena() {
  for (char* b : blocks_) delete[] b;
}

char* Arena::AllocateNewBlock(std::size_t bytes) {
  char* block = new char[bytes];
  blocks_.push_back(block);
  usage_ += bytes + sizeof(char*);
  return block;
}

char* Arena::Allocate(std::size_t bytes) {
  RIFT_CHECK(bytes > 0);
  const std::size_t padded = (bytes + (kAlign - 1)) & ~(kAlign - 1);
  if (padded <= remaining_) {
    char* out = head_;
    head_ += padded;
    remaining_ -= padded;
    return out;
  }
  // A request larger than a block gets its own block rather than wasting most
  // of a fresh one; a large DeleteRange expansion (section 8.1) makes that a
  // routine path in B1 rather than an exotic one.
  if (padded > kBlockBytes / 4) return AllocateNewBlock(padded);
  head_ = AllocateNewBlock(kBlockBytes);
  remaining_ = kBlockBytes - padded;
  char* out = head_;
  head_ += padded;
  return out;
}

}  // namespace rift
