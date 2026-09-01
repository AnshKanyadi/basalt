#include "image_fixture.h"

#include "basalt/check.h"
#include "manifest.h"
#include "table.h"
#include "table_builder.h"

namespace basalt {
namespace rig {
namespace {

// THE WHOLE SEQUENCE, IN ONE PLACE. Write, sync the file, SYNC THE DIRECTORY so
// the name is durable, open and validate, then name it in the manifest. A
// fixture that did four of these five produced a correct verdict about the
// wrong thing, twice.
void AddOne(testenv::TestEnvironment* t, const std::string& dir,
            sst::Manifest* m, sst::ManifestState* state, Slice bytes) {
  const uint64_t number = state->next_file_number;
  const std::string path = sst::TablePath(dir, number);
  {
    WritableFilePtr f;
    BASALT_CHECK(t->env()->NewWritableFile(path, &f).ok());
    BASALT_CHECK(f->Append(bytes).ok());
    BASALT_CHECK(f->Sync().ok());
    BASALT_CHECK(f->Close().ok());
  }
  {
    DirectoryPtr d;
    BASALT_CHECK(t->env()->NewDirectory(dir, &d).ok());
    BASALT_CHECK(d->Sync().ok());
    BASALT_CHECK(d->Close().ok());
  }
  std::shared_ptr<sst::Table> opened;
  BASALT_CHECK(sst::Table::Open(t->env(), path, number, &opened).ok());
  sst::TableMeta meta;
  meta.number = number;
  meta.file_bytes = opened->file_bytes();
  meta.smallest = opened->check().smallest_key;
  meta.largest = opened->check().largest_key;
  meta.largest_seq = opened->check().largest_seq;
  sst::ManifestEdit add;
  add.kind = sst::EditKind::kAddTable;
  add.table = meta;
  sst::ManifestEdit bump;
  bump.kind = sst::EditKind::kNextFileNumber;
  bump.number = number + 1;
  BASALT_CHECK(m->AppendGroup({add, bump}).ok());
  state->next_file_number = number + 1;
}

}  // namespace

testenv::DurableImage ImageHoldingTables(const std::string& dir,
                                         const std::vector<std::string>& table_bytes) {
  testenv::TestEnvironment t;
  BASALT_CHECK(t.env()->CreateDir(dir).ok());
  sst::ManifestState state;
  std::unique_ptr<sst::Manifest> m;
  std::vector<std::shared_ptr<sst::Table>> tables;
  BASALT_CHECK(sst::Manifest::Open(t.env(), dir, &state, &tables, &m).ok());
  for (const std::string& bytes : table_bytes) {
    AddOne(&t, dir, m.get(), &state, Slice(bytes));
  }
  BASALT_CHECK(m->Close().ok());
  return t.Image();
}

testenv::DurableImage BuildImage(const std::string& dir,
                                 const std::vector<std::vector<FixtureCell>>& tables) {
  // The bytes are produced in a THROWAWAY environment and then installed by the
  // path above, so there is exactly one place that knows the install sequence.
  std::vector<std::string> images;
  for (const std::vector<FixtureCell>& cells : tables) {
    testenv::TestEnvironment scratch;
    BASALT_CHECK(scratch.env()->CreateDir(dir).ok());
    const std::string path = dir + "/scratch.sst";
    {
      WritableFilePtr f;
      BASALT_CHECK(scratch.env()->NewWritableFile(path, &f).ok());
      sst::TableBuilder b(f.get());
      for (const FixtureCell& c : cells) {
        std::string k;
        AppendInternalKey(&k, Slice(c.user_key),
                          MakeTag(c.seq, c.deletion ? ValueType::kDeletion
                                                    : ValueType::kValue));
        b.Add(Slice(k), Slice(c.value));
      }
      BASALT_CHECK(b.Finish().ok());
      BASALT_CHECK(f->Sync().ok());
      BASALT_CHECK(f->Close().ok());
    }
    images.push_back(scratch.ContentNow(path));
  }
  return ImageHoldingTables(dir, images);
}

}  // namespace rig
}  // namespace basalt
