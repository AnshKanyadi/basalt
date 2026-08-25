#include "writer.h"

#include <algorithm>
#include <string>

#include "check.h"

namespace rift {
namespace wal {

Status LogWriter::EmitFragment(FragmentType type, Slice payload) {
  RIFT_CHECK(payload.size() >= 1);
  RIFT_CHECK(payload.size() <= 0xffff);
  char header[kHeaderBytes];
  const uint16_t len = static_cast<uint16_t>(payload.size());
  const uint32_t crc = FragmentCrc(len, type, payload);
  for (int i = 0; i < 4; ++i) header[i] = static_cast<char>((crc >> (8 * i)) & 0xff);
  header[4] = static_cast<char>(len & 0xff);
  header[5] = static_cast<char>((len >> 8) & 0xff);
  header[6] = static_cast<char>(static_cast<uint8_t>(type));

  Status s = file_->Append(Slice(header, kHeaderBytes));
  if (!s.ok()) return s;
  s = file_->Append(payload);
  if (!s.ok()) return s;
  offset_ += kHeaderBytes + payload.size();
  return Status::Ok();
}

Status LogWriter::AddRecord(Slice payload) {
  RIFT_CHECK(payload.size() >= 1);
  const char* p = payload.data();
  std::size_t left = payload.size();
  bool first = true;

  while (true) {
    const std::size_t block_off = static_cast<std::size_t>(offset_ % kBlockBytes);
    const std::size_t left_in_block = kBlockBytes - block_off;

    if (left_in_block < kMinFragmentBytes) {
      // EXPLICIT ZERO-FILL, not a seek. Combined with `length >= 1` and the
      // reserved type 0, this is what makes a run of zeros unmistakable for a
      // record -- and section 5.4's false-positive analysis, which is what
      // stops a normal torn tail from being reported as interior corruption,
      // rests on exactly that. Leaving the gap unwritten would leave whatever
      // the filesystem happened to have there.
      const std::string pad(left_in_block, '\0');
      Status s = file_->Append(Slice(pad));
      if (!s.ok()) return s;
      offset_ += left_in_block;
      continue;
    }

    const std::size_t room = left_in_block - kHeaderBytes;
    const std::size_t n = std::min(room, left);
    const bool last = (n == left);
    FragmentType type;
    if (first && last) {
      type = FragmentType::kFull;
    } else if (first) {
      type = FragmentType::kFirst;
    } else if (last) {
      type = FragmentType::kLast;
    } else {
      type = FragmentType::kMiddle;
    }

    Status s = EmitFragment(type, Slice(p, n));
    if (!s.ok()) return s;
    p += n;
    left -= n;
    first = false;
    if (last) return Status::Ok();
  }
}

}  // namespace wal
}  // namespace rift
