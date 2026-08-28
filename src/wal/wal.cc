#include "wal.h"

#include <utility>

#include "check.h"

namespace rift {
namespace wal {
namespace {

std::string LogName(const std::string& dir, uint64_t number) {
  std::string n = std::to_string(number);
  while (n.size() < 6) n.insert(n.begin(), '0');
  return dir + "/" + n + ".log";
}

}  // namespace

Wal::Wal(const Caps& caps, WritableFilePtr file, uint64_t file_number)
    : caps_(caps), file_(std::move(file)), file_number_(file_number) {
  writer_.reset(new LogWriter(file_.get()));
}

Wal::~Wal() = default;

Status Wal::Open(Env* env, const std::string& dir, uint64_t file_number,
                 const Caps& caps, std::unique_ptr<Wal>* out) {
  // REFUSED AT CONSTRUCTION. A cap pair that would make the tripwire fire on
  // legal input is a configuration that cannot be built, not one that is
  // documented as unwise.
  //
  // A Status rather than an abort, because the induced failure section 10.2
  // names -- "construct with kWalBufferBytes < 2 x kMaxRecordBytes;
  // construction must fail" -- has to be OBSERVED, and a path whose only
  // outcome is abort() is observable in this suite only through a death test,
  // which forks and behaves differently under three of our four sanitizer
  // lanes. Choosing INDUCIBILITY over conventional strictness is the same trade
  // Track A made when it refused checkers it could not see fail.
  //
  // THE COST: a Status can be ignored by a caller where an abort cannot, so the
  // thing that proves no caller ignores it is now load-bearing. That thing is
  // [[nodiscard]] on Status -- a compile error under -Werror, not a lane, not a
  // review habit -- and deliberately discarding one is spelled (void) at the
  // call site, which is a visible act rather than an omission.
  if (caps.max_record_bytes == 0) {
    return Status::InvalidArgument("max_record_bytes must be positive");
  }
  if (!caps.Ordered()) {
    return Status::InvalidArgument(
        "wal_buffer_bytes " + std::to_string(caps.wal_buffer_bytes) +
        " < 2 x max_record_bytes " + std::to_string(caps.max_record_bytes) +
        ": the tripwire would fire on legal input");
  }

  WritableFilePtr file;
  Status s = env->NewWritableFile(LogName(dir, file_number), &file);
  if (!s.ok()) return s;

  std::unique_ptr<Wal> w(new Wal(caps, std::move(file), file_number));
  std::string header;
  EncodeFileHeader(file_number, &header);
  s = w->writer_->AddRecord(Slice(header));
  if (!s.ok()) return s;
  s = w->file_->Flush();
  if (!s.ok()) return s;
  s = w->file_->Sync();
  if (!s.ok()) return s;

  // The directory entry, made durable before Open returns.
  DirectoryPtr d;
  s = env->NewDirectory(dir, &d);
  if (!s.ok()) return s;
  s = d->Sync();
  if (!s.ok()) return s;
  s = d->Close();
  if (!s.ok()) return s;

  *out = std::move(w);
  return Status::Ok();
}

Status Wal::Apply(SeqNum seq, const std::vector<Op>& ops) {
  // The cap is on the LOGICAL PAYLOAD, not the framed size, so the harness's
  // predicate is a sum over the ops it submitted and never has to model
  // fragmentation (section 5.3.4).
  const uint64_t bytes = BatchRecordBytes(ops);
  if (bytes > caps_.max_record_bytes) {
    // Applies NOTHING, atomically. A partially applied over-cap batch would be
    // a torn write the format cannot represent.
    return Status::RecordTooLarge("record " + std::to_string(bytes) + " > cap " +
                                  std::to_string(caps_.max_record_bytes));
  }

  DbLock lock(mu_);
  if (closed_) return Status::InvalidArgument("Apply after Close");
  if (buffered_bytes_ + bytes > caps_.wal_buffer_bytes) {
    // A TRIPWIRE, NOT A POLICY. Unbounded growth in a fault-injected harness
    // means an OOM kill, which is the worst possible failure signal because it
    // destroys the run that would have explained it. Status::kBusy as the
    // POLICY remains the leaning for B5, under section 7.6.1's precondition.
    return Status::WalBufferFull("buffer " + std::to_string(buffered_bytes_ + bytes) +
                                 " > cap " + std::to_string(caps_.wal_buffer_bytes));
  }
  // THE POLICY, AND IT IS CHARGED AGAINST BOTH HALVES. buffered_ is what has
  // not been handed to a Sync; in_flight_ is what has been handed to one that
  // has not returned. Their sum is what the caller has submitted and the poller
  // has not drained -- the identical quantity a harness computes as
  // (submitted - drained) from its own record, which is what lets section
  // 7.6.1's predicate be stated in both directions without asking us anything.
  //
  // AFTER THE TRIPWIRE, DELIBERATELY. The cap is a last resort and must keep
  // answering first if a configuration ever puts the two in reach of each other;
  // Caps::Ordered() makes that unreachable, and this ordering means a bug in
  // that invariant surfaces as the cap firing rather than as the cap becoming
  // silently unreachable.
  if (caps_.busy_bytes != 0 &&
      buffered_bytes_ + in_flight_bytes_ + bytes > caps_.busy_bytes) {
    return Status::Busy("unsynced " +
                        std::to_string(buffered_bytes_ + in_flight_bytes_ + bytes) +
                        " > busy " + std::to_string(caps_.busy_bytes));
  }

  // COLLAPSED BEFORE ENCODING, CHARGED BEFORE COLLAPSING. The cap is computed
  // over the ops AS SUBMITTED, because section 7.6's predicate is a sum over
  // what the harness submitted and the harness does not model our collapse. The
  // encoded record is therefore never larger than the charge, only smaller --
  // conservative in the safe direction, and the two never disagree about
  // whether a batch was legal.
  std::string encoded;
  EncodeBatch(seq, CollapseBatch(ops), &encoded);
  buffered_.push_back(std::move(encoded));
  buffered_bytes_ += bytes;
  high_seq_ = seq;
  return Status::Ok();
}

Status Wal::Sync(SeqNum* watermark) {
  std::vector<std::string> batch;
  SeqNum high = 0;
  uint64_t charged = 0;
  {
    DbLock lock(mu_);
    if (closed_) return Status::InvalidArgument("Sync after Close");
    if (buffered_.empty()) {
      *watermark = durable_;
      return Status::Ok();
    }
    batch.swap(buffered_);
    // MOVED, NOT DISCARDED. These bytes are still resident and still undrained;
    // they have only changed which counter answers for them.
    charged = buffered_bytes_;
    in_flight_bytes_ += charged;
    buffered_bytes_ = 0;
    high = high_seq_;
  }
  // MUTEX RELEASED. Everything below makes Env calls, and every one of them
  // would trip the guard if the lock were still held.

  // RELEASED ON EVERY EXIT, INCLUDING THE FAILING ONES. A Sync that returns an
  // IO error has finished being in flight; the bytes are lost, not pending, and
  // a charge that survived a failed Sync would wedge the engine in permanent
  // backpressure with nothing able to clear it. Scoped rather than written out
  // at each of the five returns below, because five returns is exactly how many
  // chances there are to forget one.
  struct ReleaseInFlight {
    Wal* w;
    uint64_t n;
    ~ReleaseInFlight() {
      DbLock lock(w->mu_);
      w->in_flight_bytes_ -= n;
    }
  } release{this, charged};

  for (const std::string& rec : batch) {
    Status s = writer_->AddRecord(Slice(rec));
    if (!s.ok()) return s;
  }
  std::string group_end;
  EncodeGroupEnd(high, static_cast<uint32_t>(batch.size()), &group_end);
  Status s = writer_->AddRecord(Slice(group_end));
  if (!s.ok()) return s;
  s = file_->Flush();
  if (!s.ok()) return s;
  s = file_->Sync();
  if (!s.ok()) return s;

  // AND ONLY NOW. The watermark advances when, and only when, the Sync covering
  // this group's GROUP_END has returned success.
  {
    DbLock lock(mu_);
    durable_ = high;
  }
  *watermark = high;
  return Status::Ok();
}

SeqNum Wal::DurableSeq() const {
  DbLock lock(mu_);
  return durable_;
}

Status Wal::Close() {
  {
    DbLock lock(mu_);
    if (closed_) return Status::Ok();
    closed_ = true;
  }
  // CLOSE DOES NOT SYNC, deliberately. The watermark is the engine's only
  // durability promise; a Close that synced would make clean shutdown a hidden
  // durability event that engine/model's Close does not have, and the two
  // engines would then disagree in precisely the differential rig. The
  // consequence is a good test: close-then-reopen must be indistinguishable
  // from kill-then-reopen.
  return file_->Close();
}

}  // namespace wal
}  // namespace rift
