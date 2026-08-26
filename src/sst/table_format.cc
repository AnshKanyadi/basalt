#include "table_format.h"

#include <cstring>

#include "check.h"
#include "crc32c.h"

namespace rift {
namespace sst {
namespace {

void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::string* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
uint32_t GetU32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}
uint64_t GetU64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

}  // namespace

void EncodeHandle(const BlockHandle& h, std::string* out) {
  PutU64(out, h.offset);
  PutU32(out, h.size);
}

bool DecodeHandle(Slice s, BlockHandle* out) {
  if (s.size() != kHandleBytes) return false;
  out->offset = GetU64(s.data());
  out->size = GetU32(s.data() + 8);
  return true;
}

void EncodeFooter(const Footer& f, std::string* out) {
  const std::size_t before = out->size();
  PutU64(out, f.filter.offset);
  PutU32(out, f.filter.size);
  PutU64(out, f.index.offset);
  PutU32(out, f.index.size);
  PutU32(out, f.format_version);
  // THE RESERVE, SPENT. Zero when there is no range block, which is exactly
  // what every B2-era file already holds -- so a B2 table decodes as "no range
  // block" rather than as a newer format. That is the whole of what the reserve
  // bought, and it worked only because zero was a usable sentinel.
  PutU64(out, f.range_offset);
  out->append(kMagic, sizeof(kMagic));
  RIFT_CHECK(out->size() - before == kFooterCrcCovers);
  PutU32(out, wal::Crc32c(out->data() + before, kFooterCrcCovers));
  RIFT_CHECK(out->size() - before == kFooterBytes);
}

bool DecodeFooter(Slice image, Footer* out, std::string* why) {
  if (image.size() < kFooterBytes) {
    *why = "image is smaller than a footer";
    return false;
  }
  const char* p = image.data() + image.size() - kFooterBytes;
  // THE MAGIC IS CHECKED BEFORE THE CRC, deliberately. A foreign file is a
  // different report from a corrupted one, and telling an operator "this is not
  // an SSTable" is more useful than "this SSTable is damaged".
  if (std::memcmp(p + 36, kMagic, sizeof(kMagic)) != 0) {
    *why = "footer magic is not RIFTSST";
    return false;
  }
  const uint32_t stored = GetU32(p + kFooterCrcCovers);
  if (wal::Crc32c(p, kFooterCrcCovers) != stored) {
    *why = "footer checksum mismatch";
    return false;
  }
  out->filter.offset = GetU64(p);
  out->filter.size = GetU32(p + 8);
  out->index.offset = GetU64(p + 12);
  out->index.size = GetU32(p + 20);
  out->format_version = GetU32(p + 24);
  out->range_offset = GetU64(p + 28);
  return true;
}

void BlockBuilder::Add(Slice key, Slice value) {
  if (since_restart_ == 0) {
    restarts_.push_back(static_cast<uint32_t>(buf_.size()));
  }
  PutU32(&buf_, static_cast<uint32_t>(key.size()));
  buf_.append(key.data(), key.size());
  PutU32(&buf_, static_cast<uint32_t>(value.size()));
  buf_.append(value.data(), value.size());
  ++entries_;
  since_restart_ = (since_restart_ + 1) % kRestartInterval;
}

std::string BlockBuilder::Finish() {
  if (restarts_.empty()) restarts_.push_back(0);
  const std::size_t entries_end = buf_.size();
  for (uint32_t r : restarts_) PutU32(&buf_, r);
  PutU32(&buf_, static_cast<uint32_t>(restarts_.size()));
  PutU32(&buf_, wal::Crc32c(buf_.data(), buf_.size()));
  RIFT_CHECK(buf_.size() ==
             entries_end + restarts_.size() * 4 + 4 + kBlockTrailerBytes);
  return buf_;
}

bool ParseBlock(Slice block, std::vector<BlockEntry>* entries,
                std::vector<uint32_t>* restarts, std::string* why) {
  entries->clear();
  restarts->clear();
  if (block.size() < kBlockTrailerBytes + 4) {
    *why = "block is smaller than its own trailer";
    return false;
  }
  const char* p = block.data();
  const std::size_t n = block.size();

  const uint32_t stored = GetU32(p + n - kBlockTrailerBytes);
  if (wal::Crc32c(p, n - kBlockTrailerBytes) != stored) {
    *why = "block checksum mismatch";
    return false;
  }
  const uint32_t count = GetU32(p + n - kBlockTrailerBytes - 4);
  if (count == 0) {
    *why = "block declares zero restart points";
    return false;
  }
  // The restart array must fit between the entries and the count. Checked
  // before it is read, so a corrupt count cannot make the reader walk off the
  // block -- the same reason section 5.3.3 put the length inside the CRC.
  const std::size_t restart_bytes = static_cast<std::size_t>(count) * 4;
  if (restart_bytes + 4 + kBlockTrailerBytes > n) {
    *why = "restart array does not fit in the block";
    return false;
  }
  const std::size_t entries_end = n - kBlockTrailerBytes - 4 - restart_bytes;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t off = GetU32(p + entries_end + i * 4);
    if (off > entries_end) {
      *why = "restart offset " + std::to_string(off) + " is past the entries";
      return false;
    }
    restarts->push_back(off);
  }

  std::size_t pos = 0;
  while (pos < entries_end) {
    BlockEntry e;
    e.offset = pos;
    if (entries_end - pos < 4) { *why = "truncated key length"; return false; }
    const uint32_t klen = GetU32(p + pos);
    pos += 4;
    if (entries_end - pos < klen) { *why = "key runs past the block"; return false; }
    e.key = Slice(p + pos, klen);
    pos += klen;
    if (entries_end - pos < 4) { *why = "truncated value length"; return false; }
    const uint32_t vlen = GetU32(p + pos);
    pos += 4;
    if (entries_end - pos < vlen) { *why = "value runs past the block"; return false; }
    e.value = Slice(p + pos, vlen);
    pos += vlen;
    entries->push_back(e);
  }
  return true;
}

}  // namespace sst
}  // namespace rift
