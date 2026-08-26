// THE MANIFEST'S PURE CODEC: bytes in, structure out, and nothing else.
//
// SPLIT OUT OF manifest.h BY B3-D2a, and the split is what the rule bought on
// its first day rather than a tidy-up. The corrected oracle-independence rule
// reads:
//
//   AN ORACLE MAY PARSE THE ENGINE'S ARTIFACTS, AND MAY NOT CONSULT THE
//   ENGINE'S BELIEFS. Parsing shares a format; consulting shares a judgement.
//
// made mechanical by one mark:
//
//   AN ARTIFACT HEADER DECLARES NOTHING TAKING AN `Env*` AND NOTHING TAKING A
//   SNAPSHOT.
//
// `manifest.h` failed that mark on `Manifest::Open(Env*, ...)` -- correctly:
// opening a manifest is an ACT WITH AN OPINION about what the current state is,
// and it verifies, rotates and installs. This file is the half with no opinion,
// and it is what the drop checker parses.
//
// It mirrors `table_format.h` beside `table.h`, for the same reason and with
// the same boundary.
#ifndef RIFT_SST_MANIFEST_FORMAT_H_
#define RIFT_SST_MANIFEST_FORMAT_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "internal_key.h"
#include "slice.h"

namespace rift {
namespace sst {

// CLOSED, -Werror=switch, no default: arm. A new edit must be named before it
// can be encoded, decoded or replayed.
enum class EditKind : uint8_t {
  kInvalid = 0,
  kAddTable = 1,
  kDeleteTable = 2,
  kNextFileNumber = 3,
  kSetLogNumber = 4,
  kAddWal = 5,
  kDeleteWal = 6,
};
const char* EditKindName(EditKind kind);

struct TableMeta {
  uint64_t number = 0;
  uint64_t file_bytes = 0;
  std::string smallest;  // internal keys, as written
  std::string largest;
  // RE-DERIVED AT OPEN from the table's own bytes and compared against this.
  SeqNum largest_seq = 0;
};

struct ManifestEdit {
  EditKind kind = EditKind::kInvalid;
  TableMeta table;      // kAddTable
  uint64_t number = 0;  // kDeleteTable, kNextFileNumber, kSetLogNumber,
                        // kAddWal, kDeleteWal
};

// THERE IS NO WATERMARK FIELD HERE, AND THAT IS THE MECHANISM. A durable
// sequence arriving from anywhere has no destination in this struct.
struct ManifestState {
  std::map<uint64_t, TableMeta> tables;  // by file number: ordered, never a map
                                         // iterated for behaviour (section 6.1)
  uint64_t next_file_number = 1;

  // THE LIVE WALs, BY NUMBER, and this is B2-D5's replacement for B1's gapless
  // check: "every file number the manifest names exists, and every WAL present
  // is either named or above the manifest's counter."
  //
  // WHY THE SET AND NOT A SEQUENCE RULE. The first attempt checked that the
  // replayed WALs' batch sequences formed one contiguous run, which would have
  // caught a lost file. IT IS UNSOUND: a Write whose Apply is refused by a cap
  // CONSUMES ITS SEQUENCE AND WRITES NO BATCH -- deliberately, per the Write
  // path's own note that the contract requires monotonicity and not density --
  // so a legitimate hole is indistinguishable from a lost file, and the check
  // would refuse the normal case in the name of the abnormal one. That is the
  // inversion section 5.4 rejected candidate (a) for.
  //
  // A LOST FILE IS A QUESTION ABOUT FILES, so it is answered with file
  // identities:
  //
  //   EVERY NAMED WAL MUST EXIST. No exception. One that does not is a lost
  //   directory entry, and recovery cannot replay a prefix it cannot prove is
  //   complete.
  //
  //   A PRESENT WAL THAT IS NOT NAMED MUST HOLD NO COMMITTED BATCHES. It is
  //   ignored and deleted.
  //
  // THE ORDER IS WHAT MAKES BOTH HALVES TRUE, AND THE FIRST ATTEMPT HAD IT
  // BACKWARDS. Naming a WAL before creating it looks like the safe order and is
  // not: a crash in between leaves a name with no file, and that name PERSISTS
  // -- so "named and absent" stops meaning "lost" forever after, and the only
  // repair is an exception that then has to be justified at every later Open.
  // The kill-point sweep found it as 41 violations on the first run.
  //
  // Creating first inverts it into a window that closes itself. A WAL is
  // created, named, and only THEN written to, so a crash between creation and
  // naming leaves an EMPTY unnamed file -- which carries nothing, is provably
  // empty, and is deleted. Neither half needs an exception.
  //
  // The lowest named number is also the oldest WAL recovery must read: a flush
  // removes the names it has covered in the same manifest group that adds the
  // table covering them.
  std::set<uint64_t> wals;
};

std::string ManifestPath(const std::string& dir, uint64_t number);
std::string CurrentPath(const std::string& dir);
std::string TablePath(const std::string& dir, uint64_t number);

void EncodeEdit(const ManifestEdit& edit, std::string* out);
bool DecodeEdit(Slice payload, ManifestEdit* out, std::string* why);

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_MANIFEST_FORMAT_H_
