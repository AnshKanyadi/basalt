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

namespace {

// The ordering discipline both tombstone entry points share. Ascending by
// start, and no two with one start and one tag -- the classifier's rules,
// refused at the writer where the mistake is.
void CheckOrder(std::size_t count, Slice start, uint64_t tag,
                const std::string& last_start, uint64_t last_tag) {
  if (count == 0) return;
  const int c = start.compare(Slice(last_start));
  RIFT_CHECK(c >= 0);
  RIFT_CHECK(c != 0 || tag != last_tag);
}

}  // namespace

void TableBuilder::AddUnboundedRangeTombstone(Slice start, uint64_t tag) {
  RIFT_CHECK(!finished_);
  RIFT_CHECK(TypeOfTag(tag) == ValueType::kDeletion);
  CheckOrder(range_count_, start, tag, last_range_start_, last_range_tag_);
  std::string encoded;
  EncodeUnboundedRangeTombstone(start, tag, &encoded);
  range_.Add(Slice(encoded), Slice());
  last_range_start_.assign(start.data(), start.size());
  last_range_tag_ = tag;
  ++range_count_;
  unbounded_end_ = true;

  std::string lo;
  AppendInternalKey(&lo, start, tag);
  if (smallest_.empty() || CompareInternalKey(Slice(lo), Slice(smallest_)) < 0) {
    smallest_ = lo;
  }
  // NO `largest_` WIDENING, and that is the point: there is no finite key to
  // widen it to. `TableCheck::unbounded_end` carries the fact instead.
  const SeqNum seq = SeqOfTag(tag);
  if (seq > largest_seq_) largest_seq_ = seq;
}

void TableBuilder::AddRangeTombstone(Slice start, Slice end, uint64_t tag) {
  RIFT_CHECK(!finished_);
  // Every rule the classifier refuses on, refused HERE too -- at the writer,
  // where the mistake is. A table that reaches the classifier already broken is
  // a table nobody can act on; a RIFT_CHECK names the caller.
  RIFT_CHECK(end.compare(start) > 0);
  RIFT_CHECK(TypeOfTag(tag) == ValueType::kDeletion);
  CheckOrder(range_count_, start, tag, last_range_start_, last_range_tag_);
  std::string encoded;
  EncodeRangeTombstone(start, end, tag, &encoded);
  range_.Add(Slice(encoded), Slice());
  last_range_start_.assign(start.data(), start.size());
  last_range_tag_ = tag;
  ++range_count_;

  // THE BOUNDS THE MANIFEST RECORDS MUST ADMIT THIS TOMBSTONE. Input selection
  // reads them, and clause 2 of the drop claim is only sound if the inputs hold
  // every version of every key they contain -- a tombstone outside its own
  // table's bounds is one no compaction will read.
  //
  // The end bound is EXCLUSIVE and is widened to `end` anyway: over-covering
  // costs a file that did not need to be read, UNDER-covering resurrects data.
  // The two directions are not symmetric, so the safe one is taken.
  std::string lo;
  AppendInternalKey(&lo, start, tag);
  if (range_count_ == 1 && entries_ == 0) {
    smallest_ = lo;
  } else if (smallest_.empty() || CompareInternalKey(Slice(lo), Slice(smallest_)) < 0) {
    smallest_ = lo;
  }
  std::string hi;
  AppendInternalKey(&hi, end, tag);
  if (largest_.empty() || CompareInternalKey(Slice(hi), Slice(largest_)) > 0) {
    largest_ = hi;
  }
  const SeqNum seq = SeqOfTag(tag);
  if (seq > largest_seq_) largest_seq_ = seq;
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

  // THE RANGE BLOCK IS LAST, and the assertion below is what makes that a rule
  // rather than a habit. Its size is DERIVED from the file length, so a writer
  // that emitted anything after it would corrupt every reader's derivation
  // silently -- and a stated layout rule with no enforcement is the class this
  // project has spent six phases finding.
  uint64_t range_offset = 0;
  uint64_t range_end = 0;
  if (range_count_ > 0) {
    const std::string range = range_.Finish();
    range_offset = offset_;
    if (!Append(Slice(range)).ok()) return status_;
    range_end = offset_;
  }

  Footer footer;
  footer.filter = filter_handle;
  footer.index = index_handle;
  footer.format_version = kFormatVersion;
  footer.range_offset = range_offset;
  // NOTHING BETWEEN THE RANGE BLOCK AND THE FOOTER. This is the derivation
  // `file_size - kFooterBytes - range_offset` stated as an assertion at the one
  // place that could break it.
  RIFT_CHECK(range_offset == 0 || offset_ == range_end);
  std::string tail;
  EncodeFooter(footer, &tail);
  return Append(Slice(tail));
}

}  // namespace sst
}  // namespace rift
