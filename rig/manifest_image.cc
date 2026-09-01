#include "manifest_image.h"

#include "basalt/format.h"
#include "reader.h"

namespace basalt {
namespace rig {

bool ReplayManifestImage(Slice image, sst::ManifestState* out, std::string* why) {
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

}  // namespace rig
}  // namespace basalt
