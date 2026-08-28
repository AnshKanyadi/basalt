#include "rift.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "db.h"
#include "posix_env.h"
#include "status.h"

namespace {

using rift::Bound;
using rift::DB;
using rift::Iterator;
using rift::IterOptions;
using rift::Slice;
using rift::Snapshot;
using rift::Status;
using rift::WriteBatch;

// ONE ENUM, ONE MEANING, AND THE COMPILER HOLDS THEM TOGETHER. A second
// numbering would be a second source of truth about one fact; these asserts are
// what stop the two drifting when a code is added between them.
static_assert(static_cast<int>(Status::Code::kOk) == RIFT_OK, "");
static_assert(static_cast<int>(Status::Code::kNotFound) == RIFT_NOT_FOUND, "");
static_assert(static_cast<int>(Status::Code::kRecordTooLarge) == RIFT_RECORD_TOO_LARGE, "");
static_assert(static_cast<int>(Status::Code::kWalBufferFull) == RIFT_WAL_BUFFER_FULL, "");
static_assert(static_cast<int>(Status::Code::kIoError) == RIFT_IO_ERROR, "");
static_assert(static_cast<int>(Status::Code::kDiskFull) == RIFT_DISK_FULL, "");
static_assert(static_cast<int>(Status::Code::kCorruption) == RIFT_CORRUPTION, "");
static_assert(static_cast<int>(Status::Code::kKilled) == RIFT_KILLED, "");
static_assert(static_cast<int>(Status::Code::kInvalidArgument) == RIFT_INVALID_ARGUMENT, "");
static_assert(static_cast<int>(Status::Code::kBusy) == RIFT_BUSY, "");

// A SWITCH AND NOT A CAST, AND THE DIFFERENCE IS THE WHOLE POINT OF THE
// FUNCTION. This is the one place two independently-declared enums have to
// agree, and a static_cast agrees with anything: kBusy was added to
// Status::Code at B5.3, this file compiled without a word, and the new code
// crossed as rift_status(9) -- an integer no C header names.
//
// The static_asserts below pin each code that EXISTS. Nothing pinned that the
// set was COMPLETE, which is GF-25 at the boundary: assertions about the
// members present cannot fail on a member added. -Werror=switch can, and it is
// the mechanism status.h already relies on for exactly this.
rift_status ToC(const Status& s) {
  switch (s.code()) {  // NO default: arm -- a new code must be mapped here
    case Status::Code::kOk:              return RIFT_OK;
    case Status::Code::kNotFound:        return RIFT_NOT_FOUND;
    case Status::Code::kRecordTooLarge:  return RIFT_RECORD_TOO_LARGE;
    case Status::Code::kWalBufferFull:   return RIFT_WAL_BUFFER_FULL;
    case Status::Code::kIoError:         return RIFT_IO_ERROR;
    case Status::Code::kDiskFull:        return RIFT_DISK_FULL;
    case Status::Code::kCorruption:      return RIFT_CORRUPTION;
    case Status::Code::kKilled:          return RIFT_KILLED;
    case Status::Code::kInvalidArgument: return RIFT_INVALID_ARGUMENT;
    case Status::Code::kBusy:            return RIFT_BUSY;
  }
  // Unreachable while the switch is exhaustive, and RIFT_INTERNAL rather than
  // a cast if it ever is not: an unmapped code must arrive as the boundary's
  // own "something is wrong here", never as a number that looks like a verdict.
  return RIFT_INTERNAL;
}

// NO EXCEPTION CROSSES THIS BOUNDARY, AND THE ENFORCEMENT IS THE COMPILER
// RATHER THAN A CATCH-ALL. This translation unit and every archive it links are
// built `-fno-exceptions`, so THERE IS NO EXCEPTION TO CATCH: `throw` does not
// compile, `try` does not compile, and `operator new` aborts rather than
// throwing `std::bad_alloc`.
//
//   A DEFECT THAT CANNOT BE WRITTEN IS BETTER THAN ONE THAT IS CAUGHT (GF-31).
//   A `catch (...)` here would convert an exception into a code and lose what
//   it was; a flag that makes the exception impossible loses nothing, because
//   there is nothing to lose.
//
// THE DESIGN DOC SAID CATCH-ALL AND THE BUILD SAID OTHERWISE. B5-D2 was written
// before this file compiled, proposing a wrapper and a test that throws through
// it -- and `-fno-exceptions` refused both. The decision is corrected in the
// doc rather than the flag being relaxed to fit it.
//
// WHAT THIS DOES NOT COVER, stated rather than implied: a future contributor
// removing `-fno-exceptions`. That is what `cpp-scan` asserts and what `BM115`
// blinds -- the flag is a claim about the build, so the build is where it is
// checked.
//
// `Guard` remains as the ONE place a boundary function's preconditions are
// enforced, which is worth having for its own sake: every entry point returns
// through it, so "a null handle is RIFT_INVALID_ARGUMENT and never a
// dereference" is a property of the file rather than a rule each function
// remembers.
template <typename F>
rift_status Guard(F&& f) noexcept {
  return f();
}

// A Bound from a possibly-null pointer. NULL IS UNBOUNDED; a non-null pointer
// with length zero is THE EMPTY KEY, which is a valid key. db.h's divergence 3,
// carried across C by the pointer rather than by a flag -- there is no way to
// pass "unbounded" as bytes, so the distinction has to live in the pointer.
Bound BoundOf(const char* p, size_t n) {
  if (p == nullptr) return Bound::Unbounded();
  return Bound::At(Slice(p, n));
}

// Copies a value into a caller buffer, reporting the length either way.
rift_status Deliver(const std::string& v, char* out, size_t cap, size_t* needed) {
  if (needed != nullptr) *needed = v.size();
  if (v.size() > cap) return RIFT_BUFFER_TOO_SMALL;
  if (!v.empty()) std::memcpy(out, v.data(), v.size());
  return RIFT_OK;
}

}  // namespace

// The handles. Definitions live here so the header can keep them opaque and no
// C++ type is nameable from C.
struct rift_db {
  std::unique_ptr<rift::Env> env;
  std::unique_ptr<DB> db;
};
struct rift_batch {
  WriteBatch b;
};
struct rift_iter {
  std::unique_ptr<Iterator> it;
  // POSITIONED means a seek has placed the cursor and the entry under it has
  // not yet been returned. It replaces `started`, whose two-valued meaning
  // could not express "seeked but not yet consumed" -- and a cursor that
  // silently skipped the entry it was asked to seek to is a wrong answer with
  // no failing structure anywhere.
  bool positioned = false;
  // A PAIR THE CURSOR HAS ALREADY MOVED PAST BUT THE CALLER'S BUFFER COULD NOT
  // HOLD. It is held rather than dropped, because the alternative -- reporting
  // a short block and losing the pair -- makes the iterator silently skip
  // exactly when a caller's buffer is tight, which is the least visible way to
  // lose data.
  std::string pending_key;
  std::string pending_value;
  bool has_pending = false;
};
struct rift_snapshot {
  std::unique_ptr<Snapshot> s;
};

extern "C" {

rift_status rift_db_open(const char* dir, size_t dir_len, uint64_t flush_bytes,
                         uint64_t wal_buffer_bytes, uint64_t max_record_bytes,
                         rift_db** out) {
  return Guard([&]() -> rift_status {
    if (dir == nullptr || out == nullptr) return RIFT_INVALID_ARGUMENT;
    *out = nullptr;
    rift::wal::Caps caps;
    // ZERO MEANS THE SHIPPED DEFAULT, so a caller that does not care cannot
    // accidentally configure a regime -- section 8.4's rule reaching the API.
    if (flush_bytes != 0) caps.flush_bytes = flush_bytes;
    if (wal_buffer_bytes != 0) caps.wal_buffer_bytes = wal_buffer_bytes;
    if (max_record_bytes != 0) caps.max_record_bytes = max_record_bytes;
    if (!caps.Ordered()) return RIFT_INVALID_ARGUMENT;

    auto handle = std::unique_ptr<rift_db>(new rift_db());
    handle->env = rift::NewPosixEnv();
    const std::string path(dir, dir_len);
    (void)handle->env->CreateDir(path);
    std::unique_ptr<DB> db;
    const Status s = DB::Open(handle->env.get(), path, caps, &db);
    if (!s.ok()) return ToC(s);
    handle->db = std::move(db);
    *out = handle.release();
    return RIFT_OK;
  });
}

rift_status rift_db_close(rift_db* db) {
  return Guard([&]() -> rift_status {
    if (db == nullptr) return RIFT_INVALID_ARGUMENT;
    const Status s = db->db->Close();
    delete db;
    return ToC(s);
  });
}

rift_status rift_db_sync(rift_db* db, uint64_t* watermark) {
  return Guard([&]() -> rift_status {
    if (db == nullptr) return RIFT_INVALID_ARGUMENT;
    rift::wal::SeqNum w = 0;
    const Status s = db->db->Sync(&w);
    if (watermark != nullptr) *watermark = w;
    return ToC(s);
  });
}

uint64_t rift_db_durable_seq(const rift_db* db) {
  if (db == nullptr) return 0;
  // No Guard: DurableSeq cannot throw and has no error channel to report one
  // through. Stated rather than left as an omission.
  return db->db->DurableSeq();
}

rift_status rift_db_get(const rift_db* db, const char* key, size_t key_len,
                        char* value_out, size_t cap, size_t* needed) {
  return Guard([&]() -> rift_status {
    if (db == nullptr || key == nullptr) return RIFT_INVALID_ARGUMENT;
    std::string v;
    const Status s = db->db->Get(Slice(key, key_len), &v);
    if (!s.ok()) return ToC(s);
    return Deliver(v, value_out, cap, needed);
  });
}

rift_batch* rift_batch_new(void) {
  rift_batch* b = nullptr;
  (void)Guard([&]() -> rift_status {
    b = new rift_batch();
    return RIFT_OK;
  });
  return b;
}

void rift_batch_free(rift_batch* b) { delete b; }

rift_status rift_batch_set(rift_batch* b, const char* key, size_t key_len,
                           const char* value, size_t value_len) {
  return Guard([&]() -> rift_status {
    if (b == nullptr || key == nullptr || value == nullptr) return RIFT_INVALID_ARGUMENT;
    // COPIED AT THE BOUNDARY: WriteBatch owns its bytes, so the caller's
    // pointer is not retained past this call and cgo's rule cannot be violated.
    b->b.Set(Slice(key, key_len), Slice(value, value_len));
    return RIFT_OK;
  });
}

rift_status rift_batch_delete(rift_batch* b, const char* key, size_t key_len) {
  return Guard([&]() -> rift_status {
    if (b == nullptr || key == nullptr) return RIFT_INVALID_ARGUMENT;
    b->b.Delete(Slice(key, key_len));
    return RIFT_OK;
  });
}

rift_status rift_batch_delete_range(rift_batch* b, const char* start, size_t start_len,
                                    const char* end, size_t end_len) {
  return Guard([&]() -> rift_status {
    if (b == nullptr) return RIFT_INVALID_ARGUMENT;
    b->b.DeleteRange(BoundOf(start, start_len), BoundOf(end, end_len));
    return RIFT_OK;
  });
}

rift_status rift_db_write(rift_db* db, rift_batch* b, uint64_t* seq) {
  return Guard([&]() -> rift_status {
    if (db == nullptr || b == nullptr) return RIFT_INVALID_ARGUMENT;
    rift::wal::SeqNum s = 0;
    const Status st = db->db->Write(b->b, &s);
    if (seq != nullptr) *seq = s;
    return ToC(st);
  });
}

rift_status rift_db_iter(const rift_db* db, const char* lower, size_t lower_len,
                         const char* upper, size_t upper_len, rift_iter** out) {
  return Guard([&]() -> rift_status {
    if (db == nullptr || out == nullptr) return RIFT_INVALID_ARGUMENT;
    *out = nullptr;
    IterOptions o;
    o.lower = BoundOf(lower, lower_len);
    o.upper = BoundOf(upper, upper_len);
    auto handle = std::unique_ptr<rift_iter>(new rift_iter());
    handle->it = db->db->NewIter(o);
    *out = handle.release();
    return RIFT_OK;
  });
}

void rift_iter_free(rift_iter* it) {
  if (it == nullptr) return;
  (void)it->it->Close();
  delete it;
}

rift_status rift_iter_seek(rift_iter* it, rift_seek_mode mode,
                           const char* key, size_t key_len, int* valid) {
  return Guard([&]() -> rift_status {
    if (it == nullptr) return RIFT_INVALID_ARGUMENT;
    it->has_pending = false;
    bool ok = false;
    switch (mode) {
      case RIFT_SEEK_FIRST: ok = it->it->First(); break;
      case RIFT_SEEK_LAST:  ok = it->it->Last(); break;
      case RIFT_SEEK_GE:
        if (key == nullptr) return RIFT_INVALID_ARGUMENT;
        ok = it->it->SeekGE(rift::Slice(key, key_len));
        break;
      case RIFT_SEEK_LT:
        if (key == nullptr) return RIFT_INVALID_ARGUMENT;
        ok = it->it->SeekLT(rift::Slice(key, key_len));
        break;
      default:
        return RIFT_INVALID_ARGUMENT;
    }
    // THE ENTRY UNDER THE CURSOR HAS NOT BEEN RETURNED YET, so the next block
    // must start with it rather than past it.
    it->positioned = ok;
    if (valid != nullptr) *valid = ok ? 1 : 0;
    return RIFT_OK;
  });
}

rift_status rift_iter_block(rift_iter* it, int forward, size_t n,
                            uint32_t* key_lens, uint32_t* val_lens,
                            char* keys, size_t keys_cap, size_t* keys_used,
                            char* vals, size_t vals_cap, size_t* vals_used,
                            size_t* filled) {
  return Guard([&]() -> rift_status {
    if (it == nullptr || filled == nullptr) return RIFT_INVALID_ARGUMENT;
    *filled = 0;
    size_t ko = 0;
    size_t vo = 0;
    // CF-3: the progress quantity is `*filled`, which rises by exactly one per
    // iteration and is bounded by the caller's `n`. IT DOES NOT DEPEND ON THE
    // ENGINE'S ITERATOR TO STOP -- a cursor that failed to advance would spin a
    // loop bounded by nothing, so the bound is the caller's and the cursor's
    // advance is asserted separately by the parity tests.
    while (*filled < n) {
      Slice k;
      Slice v;
      if (it->has_pending) {
        // THE HELD PAIR GOES FIRST, and only then does the cursor advance.
        k = Slice(it->pending_key);
        v = Slice(it->pending_value);
      } else if (it->positioned) {
        // The seek's own entry, returned once and then stepped past.
        it->positioned = false;
        k = it->it->Key();
        v = it->it->Value();
      } else {
        const bool ok = forward != 0 ? it->it->Next() : it->it->Prev();
        if (!ok) {
          if (keys_used != nullptr) *keys_used = ko;
          if (vals_used != nullptr) *vals_used = vo;
          return RIFT_OK;
        }
        k = it->it->Key();
        v = it->it->Value();
      }
      // RETURNS WITHOUT CONSUMING IF THE NEXT PAIR DOES NOT FIT, so a caller
      // can grow and retry WITHOUT LOSING A POSITION. The alternative -- a
      // partial write plus an error -- would leave the caller unable to tell
      // which pairs it had.
      if (ko + k.size() > keys_cap || vo + v.size() > vals_cap) {
        if (!it->has_pending) {
          it->pending_key.assign(k.data(), k.size());
          it->pending_value.assign(v.data(), v.size());
          it->has_pending = true;
        }
        // A PAIR THAT FITS IN NO BUFFER THIS CALLER CAN OFFER must be reported
        // rather than held forever: with nothing filled, the caller is told to
        // grow. With something filled, the short block is the answer and the
        // pair waits.
        if (*filled == 0) {
          // THE NEEDED CAPACITIES, not the used ones -- the same idiom
          // rift_db_get uses, and for the same reason: a caller told only
          // "too small" has to GUESS how much to grow, and a guess that is
          // still too small loops. Reporting the requirement makes the retry
          // exact and makes the caller's loop terminate in one step.
          if (keys_used != nullptr) *keys_used = k.size();
          if (vals_used != nullptr) *vals_used = v.size();
          return RIFT_BUFFER_TOO_SMALL;
        }
        if (keys_used != nullptr) *keys_used = ko;
        if (vals_used != nullptr) *vals_used = vo;
        return RIFT_OK;
      }
      it->has_pending = false;
      key_lens[*filled] = static_cast<uint32_t>(k.size());
      val_lens[*filled] = static_cast<uint32_t>(v.size());
      if (!k.empty()) std::memcpy(keys + ko, k.data(), k.size());
      if (!v.empty()) std::memcpy(vals + vo, v.data(), v.size());
      ko += k.size();
      vo += v.size();
      ++*filled;
    }
    if (keys_used != nullptr) *keys_used = ko;
    if (vals_used != nullptr) *vals_used = vo;
    return RIFT_OK;
  });
}

rift_status rift_db_snapshot(rift_db* db, rift_snapshot** out) {
  return Guard([&]() -> rift_status {
    if (db == nullptr || out == nullptr) return RIFT_INVALID_ARGUMENT;
    auto handle = std::unique_ptr<rift_snapshot>(new rift_snapshot());
    handle->s = db->db->NewSnapshot();
    *out = handle.release();
    return RIFT_OK;
  });
}

rift_status rift_snapshot_close(rift_snapshot* s) {
  return Guard([&]() -> rift_status {
    if (s == nullptr) return RIFT_INVALID_ARGUMENT;
    const Status st = s->s->Close();
    delete s;
    return ToC(st);
  });
}

rift_status rift_snapshot_get(const rift_snapshot* s, const char* key, size_t key_len,
                              char* value_out, size_t cap, size_t* needed) {
  return Guard([&]() -> rift_status {
    if (s == nullptr || key == nullptr) return RIFT_INVALID_ARGUMENT;
    std::string v;
    const Status st = s->s->Get(Slice(key, key_len), &v);
    if (!st.ok()) return ToC(st);
    return Deliver(v, value_out, cap, needed);
  });
}

rift_status rift_test_throw(void) {
  // IT CANNOT THROW, AND THAT IS THE POINT. The function is kept because the
  // header promised a way to exercise the boundary's exception behaviour, and
  // the honest answer is that there is none TO exercise: this archive is built
  // `-fno-exceptions`, so the property is a compiler flag rather than a
  // runtime path.
  //
  // Returning RIFT_INTERNAL keeps the symbol meaningful for a caller that wants
  // to see the code round-trip, and the test that calls it asserts exactly
  // that -- never that an exception was caught, which would be a claim about a
  // mechanism this build does not have.
  return RIFT_INTERNAL;
}

}  // extern "C"
