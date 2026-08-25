#include "durable_mirror.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rift {
namespace rig {
namespace {

void PutU64(std::string* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

bool GetU64(const std::string& in, std::size_t* pos, uint64_t* v) {
  if (in.size() - *pos < 8) return false;
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) {
    x |= static_cast<uint64_t>(static_cast<unsigned char>(in[*pos + static_cast<std::size_t>(i)]))
         << (8 * i);
  }
  *pos += 8;
  *v = x;
  return true;
}

}  // namespace

bool WriteDurableMirror(const std::string& path, const testenv::DurableImage& image) {
  std::string blob;
  PutU64(&blob, image.size());
  for (const auto& kv : image) {
    PutU64(&blob, kv.first.size());
    blob += kv.first;
    PutU64(&blob, kv.second.size());
    blob += kv.second;
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  std::size_t done = 0;
  while (done < blob.size()) {
    const ssize_t w = ::write(fd, blob.data() + done, blob.size() - done);
    if (w < 0) { ::close(fd); return false; }
    done += static_cast<std::size_t>(w);
  }
  return ::close(fd) == 0;
}

bool ReadDurableMirror(const std::string& path, testenv::DurableImage* out) {
  out->clear();
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  std::string blob;
  char scratch[4096];
  while (true) {
    const ssize_t r = ::read(fd, scratch, sizeof scratch);
    if (r < 0) { ::close(fd); return false; }
    if (r == 0) break;
    blob.append(scratch, static_cast<std::size_t>(r));
  }
  ::close(fd);

  std::size_t pos = 0;
  uint64_t n = 0;
  if (!GetU64(blob, &pos, &n)) return false;
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t klen = 0, vlen = 0;
    if (!GetU64(blob, &pos, &klen) || blob.size() - pos < klen) return false;
    const std::string key = blob.substr(pos, klen);
    pos += klen;
    if (!GetU64(blob, &pos, &vlen) || blob.size() - pos < vlen) return false;
    (*out)[key] = blob.substr(pos, vlen);
    pos += vlen;
  }
  return true;
}

void MirrorHook(void* ctx, const testenv::DurableImage& image) {
  WriteDurableMirror(*static_cast<std::string*>(ctx), image);
}

}  // namespace rig
}  // namespace rift
