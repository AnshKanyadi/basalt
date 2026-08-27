#include "manifest.h"

#include <algorithm>
#include <cstdio>

#include "check.h"
#include "read_whole_file.h"
#include "reader.h"
#include "table_check.h"
#include "format.h"
#include "writer.h"

namespace rift {
namespace sst {
namespace {

void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::string* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutString(std::string* out, const std::string& s) {
  PutU32(out, static_cast<uint32_t>(s.size()));
  out->append(s);
}

class Cursor {
 public:
  explicit Cursor(Slice s) : p_(s.data()), left_(s.size()) {}
  bool U32(uint32_t* out) {
    if (left_ < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<uint32_t>(static_cast<unsigned char>(p_[i])) << (8 * i);
    }
    p_ += 4; left_ -= 4; *out = v;
    return true;
  }
  bool U64(uint64_t* out) {
    if (left_ < 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(static_cast<unsigned char>(p_[i])) << (8 * i);
    }
    p_ += 8; left_ -= 8; *out = v;
    return true;
  }
  bool U8(uint8_t* out) {
    if (left_ < 1) return false;
    *out = static_cast<uint8_t>(*p_);
    ++p_; --left_;
    return true;
  }
  bool Str(std::string* out) {
    uint32_t n = 0;
    if (!U32(&n)) return false;
    if (left_ < n) return false;
    out->assign(p_, n);
    p_ += n; left_ -= n;
    return true;
  }
  bool Done() const { return left_ == 0; }

 private:
  const char* p_;
  std::size_t left_;
};

std::string Numbered(const std::string& dir, const char* prefix, uint64_t n,
                     const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%06llu", static_cast<unsigned long long>(n));
  return dir + "/" + prefix + buf + suffix;
}

}  // namespace

const char* EditKindName(EditKind kind) {
  switch (kind) {  // NO default: arm
    case EditKind::kInvalid:         return "invalid";
    case EditKind::kAddTable:        return "add table";
    case EditKind::kDeleteTable:     return "delete table";
    case EditKind::kNextFileNumber:  return "next file number";
    case EditKind::kSetLogNumber:    return "log number";
    case EditKind::kAddWal:          return "add wal";
    case EditKind::kDeleteWal:       return "delete wal";
  }
  RIFT_UNREACHABLE("EditKind holds a value no enumerator names");
}

std::string ManifestPath(const std::string& dir, uint64_t number) {
  return Numbered(dir, "MANIFEST-", number, "");
}
std::string CurrentPath(const std::string& dir) { return dir + "/CURRENT"; }
std::string TablePath(const std::string& dir, uint64_t number) {
  return Numbered(dir, "", number, ".sst");
}

// An edit payload is [wal::RecordKind::kManifestEdit][EditKind][fields], so it
// rides inside the WAL's fragment chain and is read by the reader B1.7a
// induced. THERE IS NO WATERMARK FIELD IN ANY ARM BELOW.
void EncodeEdit(const ManifestEdit& edit, std::string* out) {
  out->push_back(static_cast<char>(wal::RecordKind::kManifestEdit));
  out->push_back(static_cast<char>(edit.kind));
  switch (edit.kind) {  // NO default: arm
    case EditKind::kAddTable:
      PutU64(out, edit.table.number);
      PutU64(out, edit.table.file_bytes);
      PutU64(out, edit.table.largest_seq);
      PutString(out, edit.table.smallest);
      PutString(out, edit.table.largest);
      out->push_back(static_cast<char>(edit.table.level));
      return;
    case EditKind::kDeleteTable:
    case EditKind::kNextFileNumber:
    case EditKind::kSetLogNumber:
    case EditKind::kAddWal:
    case EditKind::kDeleteWal:
      PutU64(out, edit.number);
      return;
    case EditKind::kInvalid:
      RIFT_UNREACHABLE("an invalid edit reached the encoder");
  }
  RIFT_UNREACHABLE("EditKind holds a value no enumerator names");
}

bool DecodeEdit(Slice payload, ManifestEdit* out, std::string* why) {
  Cursor c(payload);
  uint8_t record_kind = 0;
  uint8_t edit_kind = 0;
  if (!c.U8(&record_kind) || !c.U8(&edit_kind)) {
    *why = "edit payload is too short to carry its kinds";
    return false;
  }
  if (record_kind != static_cast<uint8_t>(wal::RecordKind::kManifestEdit)) {
    *why = "payload is not a manifest edit";
    return false;
  }
  // THE RANGE CHECK COMES FIRST, because `edit_kind` came off DISK and can hold
  // a value no enumerator names. A switch over a closed enum cannot express
  // that case -- -Werror=switch is about the enumerators that EXIST -- so the
  // one place the closed-enum discipline meets untrusted input rejects before
  // it converts, rather than converting and hoping.
  if (edit_kind == 0 || edit_kind > static_cast<uint8_t>(EditKind::kDeleteWal)) {
    *why = "unknown edit kind " + std::to_string(edit_kind);
    return false;
  }
  *out = ManifestEdit();
  out->kind = static_cast<EditKind>(edit_kind);
  switch (out->kind) {  // NO default: arm
    case EditKind::kAddTable:
      if (!c.U64(&out->table.number) || !c.U64(&out->table.file_bytes) ||
          !c.U64(&out->table.largest_seq) || !c.Str(&out->table.smallest) ||
          !c.Str(&out->table.largest) || !c.U8(&out->table.level)) {
        *why = "truncated add-table edit";
        return false;
      }
      // REFUSED, NOT CLAMPED. A level this build cannot place is a manifest
      // from a build that had more levels, and placing its file at level 1
      // would put an overlapping file into the run the read path binary
      // searches -- a wrong answer with nothing structurally wrong anywhere.
      if (out->table.level > kMaxLevel) {
        *why = "table " + std::to_string(out->table.number) + " is at level " +
               std::to_string(out->table.level) + "; this build has " +
               std::to_string(kMaxLevel + 1) + " levels";
        return false;
      }
      break;
    case EditKind::kDeleteTable:
      if (!c.U64(&out->number)) { *why = "truncated delete-table edit"; return false; }
      break;
    case EditKind::kNextFileNumber:
      if (!c.U64(&out->number)) { *why = "truncated next-file-number edit"; return false; }
      break;
    case EditKind::kSetLogNumber:
      if (!c.U64(&out->number)) { *why = "truncated log-number edit"; return false; }
      break;
    case EditKind::kAddWal:
      if (!c.U64(&out->number)) { *why = "truncated add-wal edit"; return false; }
      break;
    case EditKind::kDeleteWal:
      if (!c.U64(&out->number)) { *why = "truncated delete-wal edit"; return false; }
      break;
    case EditKind::kInvalid:
      RIFT_UNREACHABLE("the range check above admitted kInvalid");
  }
  if (!c.Done()) {
    *why = "trailing bytes after a complete edit";
    return false;
  }
  return true;
}


// ---------------------------------------------------------------- the class

namespace {

// Replays one manifest image into `state`. Pure once the bytes are in hand,
// which is what lets the tests drive every refusal below from a hand-built file
// -- the same shape as the SSTable classifier, for the same reason.
Status Replay(const std::string& path, uint64_t expected_number, Slice image,
              ManifestState* state) {
  const wal::ScanResult scan = wal::ScanLog(image);
  if (scan.outcome == wal::ScanOutcome::kInteriorCorruption) {
    return Status::Corruption(
        path + ": interior corruption at block " +
        std::to_string(scan.failure_block) + ", byte offset " +
        std::to_string(scan.failure_offset) + " (" + scan.failure_reason +
        "); a structurally valid record follows at offset " +
        std::to_string(scan.resync_offset));
  }
  bool saw_header = false;
  for (std::size_t i = 0; i < scan.committed_count; ++i) {
    const wal::LogicalRecord& rec = scan.records[i];
    switch (rec.kind) {  // NO default: arm
      case wal::RecordKind::kFileHeader: {
        if (saw_header) return Status::Corruption(path + ": a second file header");
        wal::DecodedFileHeader h;
        if (!wal::DecodeFileHeader(Slice(rec.payload), &h)) {
          return Status::Corruption(path + ": malformed file header");
        }
        // The file's own record of its number must agree with the name it was
        // found under. A manifest reachable through the wrong name is a CURRENT
        // that survived a rename it should not have.
        if (h.file_number != expected_number) {
          return Status::Corruption(
              path + ": file header says number " + std::to_string(h.file_number));
        }
        saw_header = true;
        break;
      }
      case wal::RecordKind::kBatch:
        // A WAL RECORD IN A MANIFEST. The two logs share a physical format on
        // purpose and are told apart by the kinds they hold; each refuses the
        // other's, and this is one half of that pair.
        return Status::Corruption(path + ": a WAL batch record in a manifest");
      case wal::RecordKind::kGroupEnd: {
        wal::DecodedGroupEnd g;
        if (!wal::DecodeGroupEnd(Slice(rec.payload), &g)) {
          return Status::Corruption(path + ": malformed group terminator");
        }
        // D7'S FORWARD BINDING, ENFORCED WHERE IT CAN BE. The shared terminator
        // has a sequence field; a manifest writes zero into it and a manifest
        // reader refuses anything else, so no number can enter this file's
        // state through the one field that has the shape of a watermark.
        if (g.high_seq != 0) {
          return Status::Corruption(
              path + ": a manifest group terminator carries sequence " +
              std::to_string(g.high_seq) +
              "; the manifest may not record a durable sequence");
        }
        break;
      }
      case wal::RecordKind::kManifestEdit: {
        if (!saw_header) return Status::Corruption(path + ": an edit before the file header");
        ManifestEdit edit;
        std::string why;
        if (!DecodeEdit(Slice(rec.payload), &edit, &why)) {
          return Status::Corruption(path + ": " + why);
        }
        switch (edit.kind) {  // NO default: arm
          case EditKind::kAddTable:
            state->tables[edit.table.number] = edit.table;
            break;
          case EditKind::kDeleteTable:
            state->tables.erase(edit.number);
            break;
          case EditKind::kNextFileNumber:
            state->next_file_number = edit.number;
            break;
          case EditKind::kSetLogNumber:
            // RESERVED AND NEVER WRITTEN. It was the first shape of the live-WAL
            // rule and the set replaced it; the byte stays spent so a manifest
            // written by a build that used it is still decodable.
            break;
          case EditKind::kAddWal:
            state->wals.insert(edit.number);
            break;
          case EditKind::kDeleteWal:
            state->wals.erase(edit.number);
            break;
          case EditKind::kInvalid:
            RIFT_UNREACHABLE("DecodeEdit returned kInvalid and reported success");
        }
        break;
      }
      case wal::RecordKind::kInvalid:
        RIFT_UNREACHABLE("ScanLog returned a record of the reserved kind");
    }
  }
  if (!saw_header) return Status::Corruption(path + ": no file header");
  return Status::Ok();
}

// L1 IS A RUN, AND THE OPEN REFUSES A MANIFEST THAT SAYS OTHERWISE.
//
// The same precondition `ConcatIter` asserts, checked HERE because the two
// arrive from different places and only one of them can be answered with an
// abort. In-process, an overlapping run is a bug in this build and RIFT_CHECK
// is the right response. FROM A MANIFEST ON DISK it is untrusted input, and a
// process that aborts on a damaged file cannot report what is wrong with it.
//
// What it costs to skip: two L1 files holding the same user key means the
// binary search finds one of them and the other's version is INVISIBLE -- a
// deletion that stops hiding a value, which reads as data returning from the
// dead and has no structural signature at all.
Status VerifyL1IsARun(const ManifestState& state,
                      const std::vector<std::shared_ptr<Table>>& opened) {
  // OVER THE OPENED TABLES AS WELL AS THE METAS, and that is B3.5e's change.
  // Whether a bound is EXCLUSIVE, and whether a table reaches to infinity, are
  // derived from the table's own bytes -- deliberately NOT manifest fields, so
  // D4 §5.1 point 2 is satisfied by not adding a number rather than by adding
  // one carefully. The check therefore has to look where the facts live.
  std::map<uint64_t, const Table*> by_number;
  for (const auto& t : opened) by_number[t->number()] = t.get();

  std::vector<const Table*> l1;
  for (const auto& entry : state.tables) {
    if (entry.second.level != 1) continue;
    const auto it = by_number.find(entry.first);
    if (it == by_number.end()) continue;
    l1.push_back(it->second);
  }
  std::sort(l1.begin(), l1.end(), [](const Table* a, const Table* b) {
    const int c = ExtractUserKey(Slice(a->check().smallest_key))
                      .compare(ExtractUserKey(Slice(b->check().smallest_key)));
    if (c != 0) return c < 0;
    return a->number() < b->number();
  });

  for (std::size_t i = 1; i < l1.size(); ++i) {
    const Table* prev = l1[i - 1];
    const Table* next = l1[i];
    const Slice prev_last = ExtractUserKey(Slice(prev->check().largest_key));
    const Slice next_first = ExtractUserKey(Slice(next->check().smallest_key));
    const int c = prev_last.compare(next_first);
    if (c > 0) {
      return Status::Corruption(
          "level 1 is not a run: table " + std::to_string(prev->number()) +
          " ends at a user key after where table " +
          std::to_string(next->number()) + " begins");
    }
    // TOUCHING IS LEGAL EXACTLY WHEN THE FIRST BOUND IS EXCLUSIVE. Splitting a
    // tombstone at an output boundary produces `[.., B)` and `[B, ..)`, which
    // name B in both bounds and share no key at all.
    if (c == 0 && !prev->check().largest_is_exclusive) {
      return Status::Corruption(
          "level 1 is not a run: table " + std::to_string(prev->number()) +
          " and table " + std::to_string(next->number()) +
          " both hold the user key their bounds meet at");
    }
  }
  // AND ONLY THE LAST MAY REACH TO INFINITY, for the reason db.cc's install
  // path states: an unbounded tombstone anywhere else is invisible to every
  // read that does not land on its file, so the range delete silently stops
  // applying above that file's bounds.
  for (std::size_t i = 0; i + 1 < l1.size(); ++i) {
    if (l1[i]->check().unbounded_end) {
      return Status::Corruption(
          "level 1 file " + std::to_string(l1[i]->number()) +
          " holds a range tombstone with no upper bound and is not the last of "
          "the run: every read that does not land on this file would miss it");
    }
  }
  return Status::Ok();
}

// D4 SECTION 5.1 POINT 2, AND IT IS THE WHOLE OF IT: every sequence the
// manifest records is checked against the file that justifies it. The table is
// validated by the SAME ValidateTable the classifier's gates were induced
// against, and its largest key -- which is a fact about bytes on disk -- is what
// the manifest's number is held to.
Status VerifyTables(Env* env, const std::string& dir, const ManifestState& state,
                    std::vector<std::shared_ptr<Table>>* opened) {
  // NEWEST FIRST once built: std::map iterates by ascending file number, so the
  // vector is reversed at the end. Order is not cosmetic -- a deletion in a
  // newer table must hide a value in an older one.
  std::vector<std::shared_ptr<Table>> built;
  for (const auto& entry : state.tables) {
    const TableMeta& meta = entry.second;
    const std::string path = TablePath(dir, meta.number);
    std::shared_ptr<Table> t;
    Status s = Table::Open(env, path, meta.number, &t);
    if (!s.ok()) {
      return Status::Corruption(path + ": named by the manifest and unusable (" +
                                s.ToString() + ")");
    }
    const TableCheck& v = t->check();
    if (t->file_bytes() != meta.file_bytes) {
      return Status::Corruption(path + ": manifest says " +
                                std::to_string(meta.file_bytes) + " bytes, file has " +
                                std::to_string(t->file_bytes()));
    }
    if (v.largest_key != meta.largest || v.smallest_key != meta.smallest) {
      return Status::Corruption(path + ": key bounds disagree with the manifest");
    }
    if (v.largest_seq != meta.largest_seq) {
      return Status::Corruption(
          path + ": manifest records largest sequence " +
          std::to_string(meta.largest_seq) + "; the table's own keys say " +
          std::to_string(v.largest_seq));
    }
    built.push_back(std::move(t));
  }
  if (opened != nullptr) {
    opened->assign(built.rbegin(), built.rend());
  }
  return VerifyL1IsARun(state, built);
}

}  // namespace

Manifest::Manifest(uint64_t number, WritableFilePtr file)
    : number_(number), file_(std::move(file)),
      writer_(new wal::LogWriter(file_.get())) {}

Manifest::~Manifest() = default;

Status Manifest::AppendGroup(const std::vector<ManifestEdit>& edits) {
  RIFT_CHECK(!edits.empty());
  for (const ManifestEdit& e : edits) {
    std::string payload;
    EncodeEdit(e, &payload);
    Status s = writer_->AddRecord(Slice(payload));
    if (!s.ok()) return s;
  }
  std::string group;
  // ZERO. See the header: the shared terminator's sequence field is the one
  // watermark-shaped thing in this file, and the writer never puts a number in
  // it while the reader refuses one.
  wal::EncodeGroupEnd(0, static_cast<uint32_t>(edits.size()), &group);
  Status s = writer_->AddRecord(Slice(group));
  if (!s.ok()) return s;
  return file_->Sync();
}

Status Manifest::Close() {
  if (file_ == nullptr) return Status::Ok();
  Status s = file_->Close();
  file_.reset();
  writer_.reset();
  return s;
}

namespace {

// Reads CURRENT strictly. A lenient parse here is a database that opens the
// wrong manifest because a byte changed, which is the one file in the tree
// whose whole job is to name another one.
Status ReadCurrent(Env* env, const std::string& dir, uint64_t* number) {
  std::string body;
  Status s = ReadWholeFile(env, CurrentPath(dir), &body);
  if (!s.ok()) return s;
  const std::string prefix = "MANIFEST-";
  if (body.size() != prefix.size() + 6 + 1 || body.compare(0, prefix.size(), prefix) != 0 ||
      body[body.size() - 1] != '\n') {
    return Status::Corruption(CurrentPath(dir) + ": not a manifest name");
  }
  uint64_t n = 0;
  for (std::size_t i = prefix.size(); i + 1 < body.size(); ++i) {
    if (body[i] < '0' || body[i] > '9') {
      return Status::Corruption(CurrentPath(dir) + ": not a manifest name");
    }
    n = n * 10 + static_cast<uint64_t>(body[i] - '0');
  }
  *number = n;
  return Status::Ok();
}

// Write-temp, sync, rename, directory sync. THE DIRECTORY SYNC IS THE POINT:
// without it the rename is durable in the page cache and absent after a power
// cut, and CURRENT names a manifest that was never installed.
Status InstallCurrent(Env* env, const std::string& dir, uint64_t number) {
  const std::string tmp = dir + "/CURRENT.tmp";
  char buf[32];
  std::snprintf(buf, sizeof buf, "MANIFEST-%06llu\n",
                static_cast<unsigned long long>(number));
  WritableFilePtr f;
  Status s = env->NewWritableFile(tmp, &f);
  if (!s.ok()) return s;
  s = f->Append(Slice(buf, std::char_traits<char>::length(buf)));
  if (s.ok()) s = f->Sync();
  const Status closed = f->Close();
  if (!s.ok()) return s;
  if (!closed.ok()) return closed;
  s = env->RenameFile(tmp, CurrentPath(dir));
  if (!s.ok()) return s;
  DirectoryPtr d;
  s = env->NewDirectory(dir, &d);
  if (!s.ok()) return s;
  s = d->Sync();
  const Status dclosed = d->Close();
  return s.ok() ? dclosed : s;
}

}  // namespace

Status Manifest::Open(Env* env, const std::string& dir, ManifestState* state,
                      std::vector<std::shared_ptr<Table>>* tables,
                      std::unique_ptr<Manifest>* out) {
  *state = ManifestState();
  if (tables != nullptr) tables->clear();
  out->reset();

  bool have_current = false;
  Status s = env->FileExists(CurrentPath(dir), &have_current);
  if (!s.ok()) return s;

  uint64_t old_number = 0;
  if (have_current) {
    s = ReadCurrent(env, dir, &old_number);
    if (!s.ok()) return s;
    const std::string path = ManifestPath(dir, old_number);
    std::string image;
    s = ReadWholeFile(env, path, &image);
    if (!s.ok()) return s;
    s = Replay(path, old_number, Slice(image), state);
    if (!s.ok()) return s;
    s = VerifyTables(env, dir, *state, tables);
    if (!s.ok()) return s;
  }

  // EVERY FILE NUMBER THE MANIFEST NAMES MUST BE BELOW THE COUNTER. Otherwise
  // the next allocation collides with a live table, and the collision is a file
  // silently overwritten rather than an error.
  for (const auto& entry : state->tables) {
    if (entry.first >= state->next_file_number) {
      return Status::Corruption(
          "manifest names table " + std::to_string(entry.first) +
          " at or above its own next file number " +
          std::to_string(state->next_file_number));
    }
  }

  const uint64_t number = state->next_file_number++;
  WritableFilePtr file;
  s = env->NewWritableFile(ManifestPath(dir, number), &file);
  if (!s.ok()) return s;
  std::unique_ptr<Manifest> m(new Manifest(number, std::move(file)));

  std::string header;
  wal::EncodeFileHeader(number, &header);
  s = m->writer_->AddRecord(Slice(header));
  if (!s.ok()) return s;

  // A FULL SNAPSHOT, as one group. The new manifest is self-contained, so
  // nothing ever has to chain backwards through retired ones.
  std::vector<ManifestEdit> snapshot;
  ManifestEdit counter;
  counter.kind = EditKind::kNextFileNumber;
  counter.number = state->next_file_number;
  snapshot.push_back(counter);
  for (uint64_t n : state->wals) {
    ManifestEdit w;
    w.kind = EditKind::kAddWal;
    w.number = n;
    snapshot.push_back(w);
  }
  for (const auto& entry : state->tables) {
    ManifestEdit add;
    add.kind = EditKind::kAddTable;
    add.table = entry.second;
    snapshot.push_back(add);
  }
  s = m->AppendGroup(snapshot);
  if (!s.ok()) return s;

  s = InstallCurrent(env, dir, number);
  if (!s.ok()) return s;

  // Only now: the old manifest is unreachable, and deleting it is idempotent --
  // B2-D5 candidate (c) folded in, so a crash before this point leaks a file
  // that the next Open removes rather than a file nothing ever removes.
  std::vector<std::string> children;
  s = env->GetChildren(dir, &children);
  if (!s.ok()) return s;
  std::sort(children.begin(), children.end());  // never iterate unsorted
  for (const std::string& name : children) {
    if (name.compare(0, 9, "MANIFEST-") != 0) continue;
    if (name == ManifestPath(dir, number).substr(dir.size() + 1)) continue;
    (void)env->DeleteFile(dir + "/" + name);
  }

  *out = std::move(m);
  return Status::Ok();
}

}  // namespace sst
}  // namespace rift
