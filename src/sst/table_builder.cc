#include "table_builder.h"

#include "check.h"

namespace rift {
namespace sst {

Status TableBuilder::Append(Slice bytes) {
  if (!status_.ok()) return status_;
  status_ = file_->Append(bytes);
  if (status_.ok()) offset_ += bytes.size();
  return status_;
}

void TableBuilder::Add(Slice internal_key, Slice value) {
  RIFT_CHECK(!finished_);
  RIFT_CHECK(internal_key.size() >= kTagBytes);
  if (entries_ > 0) {
    RIFT_CHECK(CompareInternalKey(internal_key, Slice(last_key_)) > 0);
  } else {
    smallest_.assign(internal_key.data(), internal_key.size());
  }
  if (!status_.ok()) return;

  data_.Add(internal_key, value);
  // The filter is over USER keys: a lookup asks whether a user key could be in
  // this table, and an answer that depended on the caller's sequence would skip
  // nothing.
  filter_.AddUserKey(ExtractUserKey(internal_key));

  last_key_.assign(internal_key.data(), internal_key.size());
  largest_ = last_key_;
  const SeqNum seq = SeqOfTag(ExtractTag(internal_key));
  if (seq > largest_seq_) largest_seq_ = seq;
  ++entries_;
  ++block_entries_;

  if (data_.size_estimate() >= kDataBlockBytes) FlushDataBlock();
}

void TableBuilder::FlushDataBlock() {
  RIFT_CHECK(block_entries_ > 0);
  const std::string block = data_.Finish();
  BlockHandle h;
  h.offset = offset_;
  h.size = static_cast<uint32_t>(block.size());
  if (!Append(Slice(block)).ok()) return;

  // The separator is the block's LAST KEY, EXACTLY -- B2-D2. The classifier
  // asserts this equality from the bytes, which is only a check that can be
  // written because the key is not shortened.
  std::string handle;
  EncodeHandle(h, &handle);
  index_.Add(Slice(last_key_), Slice(handle));

  data_ = BlockBuilder();
  block_entries_ = 0;
}

Status TableBuilder::Finish() {
  RIFT_CHECK(!finished_);
  RIFT_CHECK(entries_ > 0);  // an empty flush SKIPS; it does not write a table
  finished_ = true;
  if (block_entries_ > 0) FlushDataBlock();
  if (!status_.ok()) return status_;

  const std::string filter = filter_.Finish();
  BlockHandle filter_handle;
  filter_handle.offset = offset_;
  filter_handle.size = static_cast<uint32_t>(filter.size());
  if (!Append(Slice(filter)).ok()) return status_;

  const std::string index = index_.Finish();
  BlockHandle index_handle;
  index_handle.offset = offset_;
  index_handle.size = static_cast<uint32_t>(index.size());
  if (!Append(Slice(index)).ok()) return status_;

  Footer footer;
  footer.filter = filter_handle;
  footer.index = index_handle;
  footer.format_version = kFormatVersion;
  std::string tail;
  EncodeFooter(footer, &tail);
  return Append(Slice(tail));
}

}  // namespace sst
}  // namespace rift
