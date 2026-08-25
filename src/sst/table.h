// AN SSTABLE, OPEN FOR READING.
//
// THE WHOLE FILE IS RESIDENT, AND THAT IS A REQUIREMENT RATHER THAN A
// SHORTCUT. B2-D7 section 8 point 1: `DeleteRange` expands at Apply, Apply
// makes NO Env CALL, and at B2 the expansion must read the merged view -- so
// every live table must be readable without touching Env. A reader that pulled
// blocks on demand would put a syscall inside Apply, which the Env-call counter
// asserts against.
//
// The cost is stated rather than discovered: memory grows with the live data
// set, so B2 is bounded by the flush threshold times the number of tables.
// B3 is where it changes, because B3 is where compaction, block-granular reads
// and a cache arrive together -- and where B2-D7's iterate-and-point-delete is
// replaced by real range tombstones, which is what removes the constraint that
// forced this in the first place.
#ifndef RIFT_SST_TABLE_H_
#define RIFT_SST_TABLE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bloom.h"
#include "env.h"
#include "internal_key.h"
#include "slice.h"
#include "status.h"
#include "table_check.h"
#include "table_format.h"

namespace rift {
namespace sst {

class Table {
 public:
  // Reads the file and VALIDATES IT with the classifier before it is usable.
  // Every table this process reads has been through the same rules that were
  // induced against hand-built bytes, so a damaged table is a refused open and
  // not a wrong answer.
  static Status Open(Env* env, const std::string& path, uint64_t number,
                     std::shared_ptr<Table>* out);

  uint64_t number() const { return number_; }
  uint64_t file_bytes() const { return image_.size(); }
  const TableCheck& check() const { return check_; }

  // "Maybe" or "definitely not". A definite no is what lets a lookup skip the
  // table entirely, and it is the only answer the filter is trusted for.
  bool MayContain(Slice user_key) const { return filter_.MayContain(user_key); }

  // The newest version at or below `snapshot`. kNotFound covers both "no such
  // key in this table" and "the newest visible version here is a deletion" --
  // and the CALLER MUST TELL THEM APART, because a deletion in a newer table
  // hides a value in an older one. `*deleted` is how.
  //
  // `*filtered` REPORTS THAT THE BLOOM ANSWERED, and it exists for one reason:
  // without it, a Get that stopped consulting the filter would return exactly
  // the same answers, slower, forever, and no lane could see it. An
  // optimisation whose absence is invisible is an optimisation nobody can
  // assert is present -- so the fact is made observable at the call rather than
  // left to a counter nothing reads.
  Status Get(Slice user_key, SeqNum snapshot, std::string* value, bool* deleted,
             bool* filtered) const;

  // A cursor over internal keys, in table order. MAKES NO Env CALL.
  class Iter {
   public:
    explicit Iter(const Table* t) : t_(t) {}
    bool Valid() const { return loaded_ && block_ < t_->blocks_.size(); }
    void SeekToFirst();
    void SeekToLast();
    // Positions at the first entry >= `target` in the internal order.
    void Seek(Slice target);
    void Next();
    void Prev();
    Slice key() const;
    Slice value() const;

   private:
    void LoadBlock(std::size_t i);
    const Table* t_;
    std::size_t block_ = 0;
    std::size_t entry_ = 0;
    std::vector<BlockEntry> entries_;
    bool loaded_ = false;
  };

 private:
  Table() = default;

  struct BlockRef {
    BlockHandle handle;
    Slice last_key;  // into image_
  };

  std::string image_;
  std::vector<BlockRef> blocks_;
  FilterReader filter_;
  TableCheck check_;
  uint64_t number_ = 0;
};

}  // namespace sst
}  // namespace rift

#endif  // RIFT_SST_TABLE_H_
