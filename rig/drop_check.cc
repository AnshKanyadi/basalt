// RIFT_ORACLE -- see drop_check.h and ORACLES.txt.
#include "drop_check.h"

#include <algorithm>
#include <vector>

#include "format.h"
#include "internal_key.h"
#include "manifest_format.h"
#include "reader.h"
#include "table_format.h"

namespace rift {
namespace rig {
namespace {

// NNNNNN.sst / NNNNNN.log / MANIFEST-NNNNNN / CURRENT, told apart by NAME.
// A path is a fact about the image the harness holds, not a question put to the
// engine.
bool ParseNumbered(const std::string& base, const char* suffix, std::size_t digits,
                   uint64_t* out) {
  const std::string suf(suffix);
  if (base.size() != digits + suf.size()) return false;
  if (base.compare(digits, suf.size(), suf) != 0) return false;
  uint64_t n = 0;
  for (std::size_t i = 0; i < digits; ++i) {
    if (base[i] < '0' || base[i] > '9') return false;
    n = n * 10 + static_cast<uint64_t>(base[i] - '0');
  }
  *out = n;
  return true;
}

std::string BaseName(const std::string& path, const std::string& dir) {
  if (path.size() > dir.size() + 1 && path.compare(0, dir.size(), dir) == 0) {
    return path.substr(dir.size() + 1);
  }
  return path;
}

// Replays a manifest image into the state it describes. PURE: the bytes are
// already in hand, and this is the same codec the engine writes with -- which
// B3-D2a permits, and B3-D2b is why it does not also supply the expectation.
bool ReplayManifest(Slice image, sst::ManifestState* out, std::string* why) {
  const wal::ScanResult scan = wal::ScanLog(image);
  if (scan.outcome == wal::ScanOutcome::kInteriorCorruption) {
    *why = "manifest: interior corruption at offset " +
           std::to_string(scan.failure_offset);
    return false;
  }
  for (std::size_t i = 0; i < scan.committed_count; ++i) {
    const wal::LogicalRecord& rec = scan.records[i];
    if (rec.kind != wal::RecordKind::kManifestEdit) continue;
    sst::ManifestEdit edit;
    std::string edit_why;
    if (!sst::DecodeEdit(Slice(rec.payload), &edit, &edit_why)) {
      *why = "manifest: " + edit_why;
      return false;
    }
    switch (edit.kind) {  // NO default: arm
      case sst::EditKind::kAddTable:       out->tables[edit.table.number] = edit.table; break;
      case sst::EditKind::kDeleteTable:    out->tables.erase(edit.number); break;
      case sst::EditKind::kNextFileNumber: out->next_file_number = edit.number; break;
      case sst::EditKind::kSetLogNumber:   break;  // reserved, never written
      case sst::EditKind::kAddWal:         out->wals.insert(edit.number); break;
      case sst::EditKind::kDeleteWal:      out->wals.erase(edit.number); break;
      case sst::EditKind::kInvalid:        *why = "manifest: invalid edit"; return false;
    }
  }
  return true;
}

// Every (user key, sequence) an SSTable image holds. Enumerating is the whole
// job, which is why `table.h` -- which also decides what a caller may see -- is
// not needed and is not admissible.
bool EnumerateTable(Slice image, std::set<VersionId>* out, std::string* why) {
  sst::Footer footer;
  if (!sst::DecodeFooter(image, &footer, why)) return false;
  std::vector<sst::BlockEntry> index;
  std::vector<uint32_t> restarts;
  const Slice index_block(image.data() + footer.index.offset, footer.index.size);
  if (!sst::ParseBlock(index_block, &index, &restarts, why)) return false;
  for (const sst::BlockEntry& ie : index) {
    sst::BlockHandle h;
    if (!sst::DecodeHandle(ie.value, &h)) { *why = "index value is not a handle"; return false; }
    std::vector<sst::BlockEntry> entries;
    std::vector<uint32_t> block_restarts;
    const Slice data(image.data() + h.offset, h.size);
    if (!sst::ParseBlock(data, &entries, &block_restarts, why)) return false;
    for (const sst::BlockEntry& e : entries) {
      if (e.key.size() < kTagBytes) { *why = "entry key is not an internal key"; return false; }
      out->insert({ExtractUserKey(e.key).ToString(), SeqOfTag(ExtractTag(e.key))});
    }
  }
  return true;
}

// Every (user key, sequence) a WAL image still holds, committed only. A record
// past the last GROUP_END was never promised and is not a survivor.
bool EnumerateWal(Slice image, std::set<VersionId>* out, std::string* why) {
  const wal::ScanResult scan = wal::ScanLog(image);
  if (scan.outcome == wal::ScanOutcome::kInteriorCorruption) {
    *why = "wal: interior corruption at offset " + std::to_string(scan.failure_offset);
    return false;
  }
  for (std::size_t i = 0; i < scan.committed_count; ++i) {
    const wal::LogicalRecord& rec = scan.records[i];
    if (rec.kind != wal::RecordKind::kBatch) continue;
    wal::DecodedBatch b;
    if (!wal::DecodeBatch(Slice(rec.payload), &b)) { *why = "wal: malformed BATCH"; return false; }
    for (const wal::Op& op : b.ops) {
      out->insert({op.key.ToString(), b.seq});
    }
  }
  return true;
}

DropVerdict Violation(DropVerdict v, const std::string& why) {
  v.outcome = RunOutcome::kContractViolation;
  v.why = why;
  return v;
}

}  // namespace

DropVerdict AdjudicateDrops(const VersionModel& model, const ImageBytes& image,
                            const std::string& dir) {
  DropVerdict v;

  // 1. WHICH FILES ARE LIVE, from CURRENT and the manifest it names. Parsed,
  //    not asked.
  const auto current = image.find(dir + "/CURRENT");
  sst::ManifestState state;
  if (current != image.end()) {
    const std::string& body = current->second;
    const std::string prefix = "MANIFEST-";
    if (body.size() != prefix.size() + 7 || body.compare(0, prefix.size(), prefix) != 0) {
      return Violation(v, "CURRENT is not a manifest name");
    }
    const auto manifest = image.find(dir + "/" + body.substr(0, body.size() - 1));
    if (manifest == image.end()) return Violation(v, "CURRENT names a manifest not in the image");
    std::string why;
    if (!ReplayManifest(Slice(manifest->second), &state, &why)) return Violation(v, why);
  }

  // 2. WHAT SURVIVED: every version the durable bytes still hold, from tables
  //    and from surviving WALs. The union is B2-Q1's partition, seen from here.
  std::set<VersionId> survived;
  for (const auto& entry : image) {
    const std::string base = BaseName(entry.first, dir);
    uint64_t n = 0;
    std::string why;
    if (ParseNumbered(base, ".sst", 6, &n)) {
      if (state.tables.find(n) == state.tables.end()) continue;  // orphan: not live
      if (!EnumerateTable(Slice(entry.second), &survived, &why)) {
        return Violation(v, entry.first + ": " + why);
      }
      v.tables_read++;
    } else if (ParseNumbered(base, ".log", 6, &n)) {
      if (!state.wals.empty() && state.wals.find(n) == state.wals.end()) continue;
      if (!EnumerateWal(Slice(entry.second), &survived, &why)) {
        return Violation(v, entry.first + ": " + why);
      }
      v.wals_read++;
    }
  }
  v.survived = survived.size();

  // 3. DIRECTION ONE -- `kept` includes everything `required`.
  const std::set<VersionId> required = model.Required();
  v.required_total = required.size();
  for (const VersionId& id : required) {
    if (survived.find(id) == survived.end()) {
      return Violation(v, "a version no reader may lose was dropped: key \"" + id.first +
                              "\" at sequence " + std::to_string(id.second) +
                              ". It is the newest version at or below an OBSERVABLE "
                              "sequence, so some snapshot -- or the current view -- "
                              "resolves to it");
    }
  }

  // 4. DIRECTION TWO -- nothing dropped that was not permitted. The only rule a
  //    single image can express is the TOMBSTONE one, and it is the one that
  //    matters: a deletion dropped while an older version of the same key
  //    survives RESURRECTS DELETED DATA.
  const std::set<VersionId> all = model.All();
  for (const VersionId& id : all) {
    if (survived.find(id) != survived.end()) continue;
    v.dropped++;
    if (!model.IsDeletion(id)) continue;
    for (const ModelVersion& older : model.VersionsOf(id.first)) {
      if (older.seq >= id.second) continue;
      if (survived.find({id.first, older.seq}) != survived.end()) {
        return Violation(v, "a deletion was dropped while an older version survived: key \"" +
                                id.first + "\", deletion at sequence " +
                                std::to_string(id.second) + ", survivor at " +
                                std::to_string(older.seq) +
                                ". Deleted data has returned");
      }
    }
  }
  return v;
}

}  // namespace rig
}  // namespace rift
