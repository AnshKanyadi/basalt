#include "recovery.h"

#include <algorithm>
#include <utility>

#include "check.h"
#include "reader.h"

namespace rift {
namespace wal {
namespace {

// Parses NNNNNN.log. Returns false for anything else, including a name that is
// numeric but not exactly six digits: a log this engine did not write is not a
// log, and guessing at one is how a foreign file becomes data.
bool ParseLogNumber(const std::string& name, uint64_t* number) {
  const std::string suffix = ".log";
  if (name.size() != 6 + suffix.size()) return false;
  if (name.compare(6, suffix.size(), suffix) != 0) return false;
  uint64_t n = 0;
  for (int i = 0; i < 6; ++i) {
    const char c = name[static_cast<std::size_t>(i)];
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = n;
  return true;
}

std::string LogPath(const std::string& dir, uint64_t number) {
  std::string n = std::to_string(number);
  while (n.size() < 6) n.insert(n.begin(), '0');
  return dir + "/" + n + ".log";
}

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

}  // namespace

Status Recover(Env* env, const std::string& dir, const Caps& caps,
               RecoveryResult* out) {
  // 1. LOCK.
  FileLockPtr lock;
  Status s = env->LockFile(dir + "/LOCK", &lock);
  if (!s.ok()) return s;

  // 2. GetChildren, parse, SORT BY PARSED NUMBER.
  //
  // Never directory order. TestEnv hands children back reverse-sorted ON
  // PURPOSE so an engine that forgot to sort fails on the first test rather
  // than on someone else's filesystem -- this is the C++ analogue of the
  // map-iteration rule, and the sort below is the whole of the response.
  std::vector<std::string> children;
  s = env->GetChildren(dir, &children);
  if (!s.ok()) return s;
  std::vector<uint64_t> numbers;
  for (const std::string& name : children) {
    uint64_t n = 0;
    if (ParseLogNumber(name, &n)) numbers.push_back(n);
  }
  std::sort(numbers.begin(), numbers.end());

  // 3. GAPLESS.
  //
  // In B1 no file is ever deleted, so a gap means a LOST DIRECTORY ENTRY -- the
  // missing-Directory::Sync bug -- and it is a hard error. This is what gives
  // the directory-sync kill point teeth; without it the loss is silent, and a
  // recovery that skipped a WAL would replay a prefix it believes is complete.
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    const uint64_t expected = i + 1;
    if (numbers[i] != expected) {
      return Status::Corruption(
          "WAL numbering is not gapless: expected " + std::to_string(expected) +
          ", found " + std::to_string(numbers[i]) +
          ". A missing WAL means a directory entry was lost, and recovery "
          "cannot replay a prefix it cannot prove is complete");
    }
  }

  out->table.reset(new MemTable());
  out->file_numbers = numbers;
  out->recovered_seq = 0;
  out->committed_batches = 0;
  out->discarded_batches = 0;

  // 4-5. Replay in order, committing group by group.
  for (uint64_t number : numbers) {
    const std::string path = LogPath(dir, number);
    std::string image;
    s = ReadWholeFile(env, path, &image);
    if (!s.ok()) return s;

    const ScanResult scan = ScanLog(Slice(image));
    switch (scan.outcome) {  // NO default: arm
      case ScanOutcome::kInteriorCorruption:
        // NO SILENT TRUNCATION, EVER. File, block, byte offset, and the
        // sequence of the last committed group -- a refused open that cannot
        // say where is one nobody can act on.
        return Status::Corruption(
            path + ": interior corruption at block " +
            std::to_string(scan.failure_block) + ", byte offset " +
            std::to_string(scan.failure_offset) + " (" + scan.failure_reason +
            "); a structurally valid record follows at offset " +
            std::to_string(scan.resync_offset) +
            "; last committed group sequence " +
            std::to_string(scan.last_committed_seq > out->recovered_seq
                               ? scan.last_committed_seq
                               : out->recovered_seq));
      case ScanOutcome::kTornTail:
      case ScanOutcome::kCleanEnd:
        break;
    }
    out->discarded_batches += scan.records.size() - scan.committed_count;

    // THE FILE_HEADER IS VALIDATED UNCONDITIONALLY, before anything else and
    // regardless of whether a group ever closed in this file.
    //
    // It was first written inside the committed-records loop, which made the
    // check conditional on a GROUP_END existing -- so a freshly created WAL
    // with the wrong name, or a foreign file, or an empty one, passed. That is
    // precisely the set section 5.3.4 says this record exists to catch, and a
    // check that only runs when unrelated state happens to be present is not a
    // check.
    if (scan.records.empty()) {
      return Status::Corruption(path + ": no records at all; not a WAL");
    }
    {
      const LogicalRecord& first = scan.records.front();
      if (first.kind != RecordKind::kFileHeader) {
        return Status::Corruption(path + ": first record is not a FILE_HEADER");
      }
      DecodedFileHeader h;
      if (!DecodeFileHeader(Slice(first.payload), &h)) {
        return Status::Corruption(path + ": malformed FILE_HEADER");
      }
      if (h.format_version != kFormatVersion) {
        return Status::Corruption(path + ": format version " +
                                  std::to_string(h.format_version));
      }
      // The name and the contents must agree. This catches a file that was
      // renamed, copied from another database, or restored out of order --
      // none of which a checksum notices, because every record in it is
      // individually perfect.
      if (h.file_number != number) {
        return Status::Corruption(path + ": FILE_HEADER says file number " +
                                  std::to_string(h.file_number));
      }
    }

    uint32_t batches_this_group = 0;
    for (std::size_t i = 0; i < scan.committed_count; ++i) {
      const LogicalRecord& rec = scan.records[i];
      switch (rec.kind) {  // NO default: arm
        case RecordKind::kInvalid:
          return Status::Corruption(path + ": a record of reserved kind 0");
        case RecordKind::kFileHeader:
          // Already validated above, unconditionally. A SECOND one is a
          // malformed log: the header is the first record of a WAL and there
          // is exactly one.
          if (i != 0) {
            return Status::Corruption(path + ": a second FILE_HEADER at record " +
                                      std::to_string(i));
          }
          break;
        case RecordKind::kBatch: {
          DecodedBatch b;
          if (!DecodeBatch(Slice(rec.payload), &b)) {
            return Status::Corruption(path + ": malformed BATCH");
          }
          for (const Op& op : b.ops) {
            switch (op.kind) {  // NO default: arm
              case OpKind::kSet:
                out->table->Add(b.seq, ValueType::kValue, op.key, op.value);
                break;
              case OpKind::kDelete:
                out->table->Add(b.seq, ValueType::kDeletion, op.key, Slice());
                break;
              case OpKind::kDeleteRange:
                // Reserved and never written before B3. Reaching it means the
                // log was written by something that is not this engine.
                return Status::Corruption(path + ": DELETE_RANGE is reserved until B3");
            }
          }
          ++batches_this_group;
          ++out->committed_batches;
          break;
        }
        case RecordKind::kGroupEnd: {
          DecodedGroupEnd g;
          if (!DecodeGroupEnd(Slice(rec.payload), &g)) {
            return Status::Corruption(path + ": malformed GROUP_END");
          }
          // batch_count is checked, which detects a dropped interior record
          // without a whole-group checksum (section 5.3.4).
          if (g.batch_count != batches_this_group) {
            return Status::Corruption(
                path + ": GROUP_END records " + std::to_string(g.batch_count) +
                " batches but " + std::to_string(batches_this_group) +
                " were read; a record was dropped from inside the group");
          }
          batches_this_group = 0;

          // THE WATERMARK IS READ, NOT INFERRED. g.high_seq is the recorded
          // field. It is not max(batch.seq), not b.seq of the last batch, and
          // not anything reconstructed from what was found. Track A's BUG-005
          // was a watermark inferred from the shape of a structure rather than
          // read from where it was written.
          if (g.high_seq < out->recovered_seq) {
            return Status::Corruption(
                path + ": GROUP_END sequence " + std::to_string(g.high_seq) +
                " goes backwards from " + std::to_string(out->recovered_seq));
          }
          out->recovered_seq = g.high_seq;
          break;
        }
      }
    }
  }

  // 6. Create WAL max+1 and make its directory entry durable BEFORE returning.
  const uint64_t next = numbers.empty() ? 1 : numbers.back() + 1;
  s = Wal::Open(env, dir, next, caps, &out->wal);
  if (!s.ok()) return s;

  (void)env->UnlockFile(std::move(lock));
  return Status::Ok();
}

}  // namespace wal
}  // namespace rift
