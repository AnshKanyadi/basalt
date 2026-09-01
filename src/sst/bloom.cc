#include "bloom.h"

#include "crc32c.h"
#include "tower.h"

namespace basalt {
namespace sst {
namespace {

void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
uint32_t GetU32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

}  // namespace

// The header carries the argument for why this class exists and why it is
// public. `h2 | 1` is not cosmetic: it is what makes step() non-zero.
ProbeWalk::ProbeWalk(uint64_t hash, uint64_t bits) : bits_(bits) {
  const uint32_t h1 = static_cast<uint32_t>(hash & 0xffffffffu);
  const uint32_t h2 = static_cast<uint32_t>(hash >> 32) | 1u;
  pos_ = h1 % bits;
  step_ = h2 % bits;
}

uint64_t ProbeWalk::Next() {
  const uint64_t p = pos_;
  pos_ += step_;  // both operands are < bits, so one subtraction suffices
  if (pos_ >= bits_) pos_ -= bits_;
  return p;
}

uint64_t BloomHash(Slice user_key) { return Fnv1a64(user_key); }

void FilterBuilder::AddUserKey(Slice user_key) {
  hashes_.push_back(BloomHash(user_key));
}

std::string FilterBuilder::Finish() const {
  std::size_t nbytes = (hashes_.size() * kBitsPerKey + 7) / 8;
  if (nbytes < kMinFilterBytes) nbytes = kMinFilterBytes;
  std::string out(nbytes, '\0');
  const uint64_t bits = static_cast<uint64_t>(nbytes) * 8;
  for (uint64_t h : hashes_) {
    ProbeWalk walk(h, bits);
    for (uint32_t i = 0; i < kProbes; ++i) {
      const uint64_t pos = walk.Next();
      out[pos / 8] = static_cast<char>(static_cast<unsigned char>(out[pos / 8]) |
                                       (1u << (pos % 8)));
    }
  }
  PutU32(&out, kProbes);
  PutU32(&out, static_cast<uint32_t>(nbytes));
  PutU32(&out, wal::Crc32c(out.data(), out.size()));
  return out;
}

bool FilterReader::Parse(Slice block, FilterReader* out, std::string* why) {
  if (block.size() < kFilterTrailerBytes + kMinFilterBytes) {
    *why = "filter block is smaller than its own trailer";
    return false;
  }
  const char* p = block.data();
  const std::size_t n = block.size();
  const uint32_t stored = GetU32(p + n - 4);
  if (wal::Crc32c(p, n - 4) != stored) {
    *why = "filter checksum mismatch";
    return false;
  }
  const uint32_t nbytes = GetU32(p + n - 8);
  const uint32_t probes = GetU32(p + n - 12);
  // THE DECLARED LENGTH MUST AGREE WITH THE BLOCK IT ARRIVED IN. This is the
  // only way a filter truncated by a torn write announces itself as a FILTER
  // problem rather than as a table that answers "no" to keys it holds.
  if (static_cast<std::size_t>(nbytes) + kFilterTrailerBytes != n) {
    *why = "filter length " + std::to_string(nbytes) + " disagrees with its block";
    return false;
  }
  if (probes == 0) {
    *why = "filter declares zero probes";
    return false;
  }
  // A BOUND ON A COUNT THAT DRIVES A LOOP. `probes` comes off disk, and a
  // corrupt four billion would be read as four billion iterations per lookup --
  // a file that is not wrong so much as unanswerable. The ceiling is loose
  // because it is not a policy: it is the point past which no honest writer
  // could have produced the number.
  if (probes > kMaxProbes) {
    *why = "filter declares " + std::to_string(probes) + " probes";
    return false;
  }
  out->bits_ = Slice(p, nbytes);
  out->probes_ = probes;
  return true;
}

bool FilterReader::MayContain(Slice user_key) const {
  const uint64_t bits = static_cast<uint64_t>(bits_.size()) * 8;
  if (bits == 0) return true;
  ProbeWalk walk(BloomHash(user_key), bits);
  for (uint32_t i = 0; i < probes_; ++i) {
    const uint64_t pos = walk.Next();
    const unsigned char byte = static_cast<unsigned char>(bits_.data()[pos / 8]);
    if ((byte & (1u << (pos % 8))) == 0) return false;
  }
  return true;
}

}  // namespace sst
}  // namespace basalt
