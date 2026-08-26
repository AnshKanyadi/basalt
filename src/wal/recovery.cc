#include "recovery.h"

#include "read_whole_file.h"

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

}  // namespace

std::string LogPath(const std::string& dir, uint64_t number) {
  std::string n = std::to_string(number);
  while (n.size() < 6) n.insert(n.begin(), '0');
  return dir + "/" + n + ".log";
}

namespace {

Status RecoverLocked(Env* env, const std::string& dir, const Caps& caps,
                     const RecoverOptions& options, RecoveryResult* out) {
  // 1. THE CALLER HOLDS THE LOCK. See recovery.h for why this function no
  //    longer takes it.
  Status s = Status::Ok();

  // 2. GetChildren, parse, SORT BY PARSED NUMBER.
  //
  // Never directory order. TestEnv hands children back reverse-sorted ON
  // PURPOSE so an engine that forgot to sort fails on the first test rather
  // than on someone else's filesystem -- this is the C++ analogue of the
  // map-iteration rule, and the sort below is the whole of the response.
  std::vector<std::string> children;
  s = env->GetChildren(dir, &children);
  if (!s.ok()) return s;
  std::vector<uint64_t> present;
  for (const std::string& name : children) {
    uint64_t n = 0;
    if (ParseLogNumber(name, &n)) present.push_back(n);
  }
  std::sort(present.begin(), present.end());

  // FILE IDENTITY IS THE HALF SEQUENCES CANNOT ANSWER. See recovery.h.
  std::vector<uint64_t> numbers;
  std::vector<uint64_t> unnamed;
  if (options.named_wals.empty()) {
    numbers = present;
  } else {
    std::vector<uint64_t> named = options.named_wals;
    std::sort(named.begin(), named.end());
    for (uint64_t n : named) {
      // EVERY NAMED WAL MUST EXIST, WITH NO EXCEPTION. The first version of
      // this rule forgave the highest named number, because naming came before
      // creating and a crash in that window left a name with no file -- and
      // that exception is what the kill-point sweep refused, 41 times. The
      // order was inverted instead; see manifest.h.
      if (std::find(present.begin(), present.end(), n) == present.end()) {
        return Status::Corruption(
            LogPath(dir, n) +
            ": named by the manifest and absent. A missing WAL means a directory "
            "entry was lost, and recovery cannot replay a prefix it cannot prove "
            "is complete");
      }
      numbers.push_back(n);
    }
    for (uint64_t n : present) {
      if (std::find(named.begin(), named.end(), n) == named.end()) unnamed.push_back(n);
    }
    out->obsolete_wals = unnamed.size();
  }

  // A PRESENT, UNNAMED WAL IS ONE OF EXACTLY TWO THINGS, and both are legal:
  //
  //   caught between creation and naming -- it is EMPTY, because nothing is
  //   written to a WAL before its name is durable; or
  //
  //   RETIRED BY A FLUSH and not yet deleted -- the manifest dropped its name
  //   in the same group that added the table covering it, and the crash landed
  //   before the file was removed.
  //
  // What both have in common is the property that matters, so it is the
  // property checked: NOTHING IN AN UNNAMED WAL MAY BE ABOVE WHAT THE TABLES
  // COVER. That is B2-Q1's "nothing covered twice" from the other side -- a
  // record above S in a file nobody names is a record with no durable home --
  // and the kill-point sweep is what turned it from an assumption about
  // emptiness into this.
  for (uint64_t n : unnamed) {
    const std::string path = LogPath(dir, n);
    std::string image;
    s = ReadWholeFile(env, path, &image);
    if (!s.ok()) return s;
    const ScanResult scan = ScanLog(Slice(image));
    for (std::size_t i = 0; i < scan.committed_count; ++i) {
      if (scan.records[i].kind != RecordKind::kBatch) continue;
      DecodedBatch b;
      if (!DecodeBatch(Slice(scan.records[i].payload), &b)) {
        return Status::Corruption(path + ": malformed BATCH in an unnamed WAL");
      }
      if (b.seq > options.covered_through) {
        return Status::Corruption(
            path + ": present, not named by the manifest, and holding a batch "
            "at sequence " + std::to_string(b.seq) +
            " above what the SSTables cover (" +
            std::to_string(options.covered_through) +
            "). A record with no durable home would be lost by ignoring it");
      }
    }
  }

  // 3. B2-Q1's partition check replaces B1's file-number gapless check. The
  //    spans are collected during replay below and adjudicated after it, since
  //    the property is about SEQUENCES and sequences are only known once the
  //    records have been read. See recovery.h for the invariant in full.
  struct Span {
    uint64_t number = 0;
    bool any = false;
    SeqNum first = 0;
    SeqNum last = 0;
  };
  std::vector<Span> spans;

  out->table.reset(new MemTable());
  out->file_numbers = numbers;
  out->recovered_seq = 0;
  out->committed_batches = 0;
  out->discarded_batches = 0;

  // 4-5. Replay in order, committing group by group.
  for (uint64_t number : numbers) {
    Span span;
    span.number = number;
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
          // NOTHING COVERED TWICE, ASSERTED RATHER THAN REPAIRED. File
          // selection above already excluded every WAL a table covers, so a
          // batch arriving here at or below S means the selection was wrong --
          // and repairing it by skipping would hide exactly that. BM55 removes
          // the selection and this is what fires.
          if (b.seq <= options.covered_through) {
            return Status::Corruption(
                path + ": batch at sequence " + std::to_string(b.seq) +
                " is already covered by the SSTables through " +
                std::to_string(options.covered_through) +
                "; a WAL below the manifest's log number was replayed");
          }
          if (!span.any) { span.any = true; span.first = b.seq; }
          span.last = b.seq;
          for (const Op& op : b.ops) {
            switch (op.kind) {  // NO default: arm
              case OpKind::kSet:
                out->table->Add(b.seq, ValueType::kValue, op.key, op.value);
                break;
              case OpKind::kDelete:
                out->table->Add(b.seq, ValueType::kDeletion, op.key, Slice());
                break;
              case OpKind::kDeleteRange:
                // B3.5: REPLAY INSERTS IT AND COMPUTES NOTHING, which is the
                // whole reason section 8.1's circularity is gone. B2 recorded
                // the EXPANSION because replaying a raw DeleteRange would have
                // meant expanding it against a state recovery was still
                // rebuilding. A range tombstone means the same thing wherever
                // it is replayed: it hides every version below its own
                // sequence, and nothing about the surrounding state enters in.
                //
                // AN EMPTY END MEANS UNBOUNDED. `Op::value` carries the end
                // key, and an empty one never has a legal finite meaning --
                // an empty or inverted range covers nothing and is dropped
                // before it is ever written.
                out->table->AddRangeTombstone(b.seq, op.key, op.value,
                                              op.value.empty());
                break;
            }
          }
          ++batches_this_group;
          ++out->committed_batches;
          break;
        }
        case RecordKind::kManifestEdit:
          // The other half of the pair: a manifest presented as a WAL. The two
          // logs share a physical format deliberately and are distinguished by
          // the kinds they hold, so each must refuse the other's.
          return Status::Corruption(path + ": a manifest edit in a WAL");
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
    spans.push_back(span);
  }

  // 3 (continued). THE PARTITION, ADJUDICATED. What sequences can answer is
  //    ORDER and the JOIN; file identity answered the rest, above.
  {
    const SeqNum covered = options.covered_through;
    SeqNum chain_end = 0;
    bool seen_any = false;
    for (const Span& span : spans) {
      if (!span.any) continue;
      if (!seen_any) {
        // THE JOIN. The first replayed batch must be above everything the
        // tables hold; equality would be a record covered twice.
        if (span.first <= covered) {
          return Status::Corruption(
              LogPath(dir, span.number) + ": first batch is sequence " +
              std::to_string(span.first) +
              ", at or below what the SSTables already cover (" +
              std::to_string(covered) + ")");
        }
        out->first_wal_seq = span.first;
      } else if (span.first <= chain_end) {
        // ORDER. Files are replayed oldest first, so a later file whose first
        // batch does not exceed the previous file's last is a log written out
        // of order -- which no crash produces.
        return Status::Corruption(
            LogPath(dir, span.number) + ": first batch is sequence " +
            std::to_string(span.first) + ", not above the previous WAL's last (" +
            std::to_string(chain_end) + ")");
      }
      chain_end = span.last;
      out->last_wal_seq = span.last;
      seen_any = true;
    }
    // The tables never claim more than the watermark.
    if (covered > out->recovered_seq) out->recovered_seq = covered;
  }

  // 6. Create the fresh WAL at the number the MANIFEST supplied, and make its
  //    directory entry durable BEFORE returning. B1's max+1 expires here.
  s = Wal::Open(env, dir, options.next_file_number, caps, &out->wal);
  if (!s.ok()) return s;
  return Status::Ok();
}

}  // namespace

Status Recover(Env* env, const std::string& dir, const Caps& caps,
               const RecoverOptions& options, RecoveryResult* out) {
  return RecoverLocked(env, dir, caps, options, out);
}

}  // namespace wal
}  // namespace rift
