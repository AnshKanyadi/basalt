#include "read_whole_file.h"

namespace basalt {

Status ReadWholeFile(Env* env, const std::string& path, std::string* out) {
  SequentialFilePtr f;
  Status s = env->NewSequentialFile(path, &f);
  if (!s.ok()) return s;
  out->clear();
  char scratch[16384];
  while (true) {
    Slice chunk;
    s = f->Read(sizeof scratch, &chunk, scratch);
    if (!s.ok()) { (void)f->Close(); return s; }
    if (chunk.empty()) break;
    out->append(chunk.data(), chunk.size());
  }
  return f->Close();
}

}  // namespace basalt
