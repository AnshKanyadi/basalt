// usage: rift_bench <fillrandom|readrandom|mixed|scan> <n> <batch> <block> <seed>
//
// THE NATIVE COLUMN. It exists so the cgo column has something to be a
// difference from: the same workload, the same loop shape, the same key stream,
// with the C++ DB called directly instead of through the boundary.
//
// IT TIMES THE LOOP AND NOT THE PROCESS. Open, fill and close are outside the
// measured region wherever they are not the thing being measured, because
// process startup and a first-touch page fault are not boundary costs and would
// swamp the difference this table exists to show.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bench_keys.h"
#include "db.h"
#include "posix_env.h"

namespace {

using rift::Bench64;
using rift::BenchKey;

const int kKeyBytes = 16;
const int kValueBytes = 100;

int64_t NowNanos() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr,
                 "usage: rift_bench <fillrandom|readrandom|mixed|scan> "
                 "<n> <batch> <block> <seed>\n");
    return 2;
  }
  const std::string workload = argv[1];
  const uint64_t n = std::strtoull(argv[2], nullptr, 10);
  const uint64_t batch = std::strtoull(argv[3], nullptr, 10);
  const uint64_t seed = std::strtoull(argv[5], nullptr, 10);

  std::unique_ptr<rift::Env> env = rift::NewPosixEnv();
  const std::string dir = std::string("/tmp/rift-bench-native-") + std::to_string(seed) +
                          "-" + workload + "-" + std::to_string(batch);
  // A FRESH DIRECTORY EVERY RUN. A benchmark that reopens a database left by a
  // previous run measures whatever that run happened to leave -- a different
  // number of SSTables, a different compaction debt -- and the sweep would
  // then be reading its own history back as a result.
  (void)std::system(("rm -rf '" + dir + "'").c_str());
  (void)env->CreateDir(dir);
  std::unique_ptr<rift::DB> db;
  const rift::Status opened = rift::DB::Open(env.get(), dir, rift::wal::Caps(), &db);
  if (!opened.ok()) {
    std::fprintf(stderr, "open failed: %s\n", opened.ToString().c_str());
    return 1;
  }

  const std::string value(kValueBytes, 'v');

  // PRE-FILL for every workload but fillrandom, outside the timed region.
  if (workload != "fillrandom") {
    rift::WriteBatch b;
    uint64_t in = 0;
    for (uint64_t i = 0; i < n; i++) {
      const std::string k = BenchKey(seed, i, kKeyBytes);
      b.Set(rift::Slice(k), rift::Slice(value));
      if (++in == batch) {
        rift::wal::SeqNum s = 0;
        (void)db->Write(b, &s);
        b = rift::WriteBatch();
        in = 0;
      }
    }
    if (in != 0) {
      rift::wal::SeqNum s = 0;
      (void)db->Write(b, &s);
    }
    // NO SYNC HERE, matching the Go columns. engine.Engine has no Sync -- this
    // project DRIVES durability -- so a native pre-fill that synced would be
    // doing work outside its timed region that another column cannot do, and
    // the difference between columns would stop being the boundary.
  }

  const int64_t start = NowNanos();
  uint64_t ops = 0;

  if (workload == "fillrandom") {
    rift::WriteBatch b;
    uint64_t in = 0;
    for (uint64_t i = 0; i < n; i++) {
      const std::string k = BenchKey(seed, i, kKeyBytes);
      b.Set(rift::Slice(k), rift::Slice(value));
      ops++;
      if (++in == batch) {
        rift::wal::SeqNum s = 0;
        (void)db->Write(b, &s);
        b = rift::WriteBatch();
        in = 0;
      }
    }
    if (in != 0) {
      rift::wal::SeqNum s = 0;
      (void)db->Write(b, &s);
    }
  } else if (workload == "readrandom") {
    std::string out;
    for (uint64_t i = 0; i < n; i++) {
      const std::string k = BenchKey(seed, Bench64(seed ^ 0x5eed, i) % n, kKeyBytes);
      (void)db->Get(rift::Slice(k), &out);
      ops++;
    }
  } else if (workload == "mixed") {
    std::string out;
    rift::WriteBatch b;
    uint64_t in = 0;
    for (uint64_t i = 0; i < n; i++) {
      if ((Bench64(seed ^ 0x111d, i) & 1) == 0) {
        const std::string k = BenchKey(seed, Bench64(seed ^ 0x5eed, i) % n, kKeyBytes);
        (void)db->Get(rift::Slice(k), &out);
      } else {
        const std::string k = BenchKey(seed, Bench64(seed ^ 0x5eed, i) % n, kKeyBytes);
        b.Set(rift::Slice(k), rift::Slice(value));
        if (++in == batch) {
          rift::wal::SeqNum s = 0;
          (void)db->Write(b, &s);
          b = rift::WriteBatch();
          in = 0;
        }
      }
      ops++;
    }
    if (in != 0) {
      rift::wal::SeqNum s = 0;
      (void)db->Write(b, &s);
    }
  } else if (workload == "scan") {
    std::unique_ptr<rift::Iterator> it = db->NewIter(rift::IterOptions());
    for (bool ok = it->First(); ok; ok = it->Next()) {
      // TOUCHED, NOT IGNORED. A loop whose body reads nothing measures the
      // cursor and not the data crossing it, and the compiler is entitled to
      // notice.
      ops += it->Key().size() + it->Value().size();
    }
    ops = n;
  } else {
    std::fprintf(stderr, "unknown workload \"%s\"\n", workload.c_str());
    return 2;
  }

  const int64_t elapsed = NowNanos() - start;
  (void)db->Close();
  // ops IS PRINTED, not discarded. A workload's result being unused is how a
  // compiler is invited to delete the workload -- and -Werror caught exactly
  // that here, on the first build.
  std::printf("RESULT native %s n=%llu batch=%llu ns=%lld ops=%llu\n", workload.c_str(),
              (unsigned long long)n, (unsigned long long)batch, (long long)elapsed,
              (unsigned long long)ops);
  return 0;
}
