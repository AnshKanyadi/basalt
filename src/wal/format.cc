#include "format.h"

#include <cstring>
#include <string>

#include "check.h"
#include "crc32c.h"

namespace rift {
namespace wal {
namespace {

void PutU8(std::string* out, uint8_t v) { out->push_back(static_cast<char>(v)); }
void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::string* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutSlice(std::string* out, Slice s) {
  PutU32(out, static_cast<uint32_t>(s.size()));
  out->append(s.data(), s.size());
}

uint64_t GetU64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

}  // namespace

uint64_t BatchRecordBytes(const std::vector<Op>& ops) {
  uint64_t n = 1 + 8 + 4;  // kind, seq, op_count
  for (const Op& op : ops) {
    n += 1 + 4 + op.key.size();  // op_kind, key_len, key
    switch (op.kind) {           // NO default: arm
      case OpKind::kSet:
      case OpKind::kDeleteRange:
        n += 4 + op.value.size();
        break;
      case OpKind::kDelete:
        break;
    }
  }
  return n;
}

void EncodeBatch(SeqNum seq, const std::vector<Op>& ops, std::string* out) {
  const std::size_t before = out->size();
  PutU8(out, static_cast<uint8_t>(RecordKind::kBatch));
  PutU64(out, seq);
  PutU32(out, static_cast<uint32_t>(ops.size()));
  for (const Op& op : ops) {
    PutU8(out, static_cast<uint8_t>(op.kind));
    PutSlice(out, op.key);
    switch (op.kind) {  // NO default: arm
      case OpKind::kSet:
      case OpKind::kDeleteRange:
        PutSlice(out, op.value);
        break;
      case OpKind::kDelete:
        break;
    }
  }
  // The encoder and the frozen formula must agree, and this is the cheapest
  // place to find out that they do not: the cap is adjudicated against the
  // formula, so an encoder that wrote more bytes than the formula counts would
  // let an over-cap record through with the harness computing it as legal.
  RIFT_CHECK(out->size() - before == BatchRecordBytes(ops));
}

void EncodeGroupEnd(SeqNum high_seq, uint32_t batch_count, std::string* out) {
  PutU8(out, static_cast<uint8_t>(RecordKind::kGroupEnd));
  PutU64(out, high_seq);
  PutU32(out, batch_count);
}

void EncodeFileHeader(uint64_t file_number, std::string* out) {
  // The FILE_HEADER is the first LOGICAL RECORD of every WAL rather than a raw
  // file header, so block arithmetic still starts at offset 0. It catches an
  // empty file, a truncated file, a foreign file, and a file whose name and
  // contents disagree.
  PutU8(out, static_cast<uint8_t>(RecordKind::kFileHeader));
  out->append(kMagic, sizeof(kMagic));
  PutU32(out, kFormatVersion);
  PutU64(out, file_number);
}

uint32_t FragmentCrc(uint16_t length, FragmentType type, Slice payload) {
  // length || type || payload. See the header for why the length is in here and
  // what breaks if somebody takes it out.
  std::string buf;
  buf.reserve(3 + payload.size());
  buf.push_back(static_cast<char>(length & 0xff));
  buf.push_back(static_cast<char>((length >> 8) & 0xff));
  buf.push_back(static_cast<char>(static_cast<uint8_t>(type)));
  buf.append(payload.data(), payload.size());
  return Crc32c(buf.data(), buf.size());
}

bool PeekKindAndSeq(Slice payload, RecordKind* kind, SeqNum* seq) {
  if (payload.size() < 1) return false;
  const uint8_t k = static_cast<uint8_t>(payload.data()[0]);
  switch (static_cast<RecordKind>(k)) {  // NO default: arm
    case RecordKind::kInvalid:
      return false;
    case RecordKind::kBatch:
    case RecordKind::kGroupEnd:
      if (payload.size() < 9) return false;
      *kind = static_cast<RecordKind>(k);
      *seq = GetU64(payload.data() + 1);
      return true;
    case RecordKind::kFileHeader:
      *kind = RecordKind::kFileHeader;
      *seq = 0;
      return true;
  }
  return false;  // k is not a named enumerator at all
}

}  // namespace wal
}  // namespace rift
